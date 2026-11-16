#include "esp8266.h"
#include "usart.h"
#include "gpio.h"
#include "dma.h"
#include "flash.h"
#include <stdio.h>
#include <string.h>
/* ---------------- 外部变量 ---------------- */
extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart3_rx;     
extern volatile uint8_t   is_network_ready;    /* net.c 里定义：1 = 可以收 OTA 数据了 */

/* ---------------- 全局变量 ---------------- */
uint8_t          ESP8266_RxBuff[UART_RX_BUFF_LEN];   /* AT 回复累积缓冲 */
volatile uint8_t ESP_RX_Complete = 0;                /* 收到一段数据的标志 */

static uint8_t  it_buf[UART_RX_BUFF_LEN];       /* HAL 每次收进这里，回调里再追加到 RxBuff */
static uint16_t rx_len = 0;                     /* RxBuff 里已经攒了多少字节 */
static uint8_t  Debug_RxBuff[UART_RX_BUFF_LEN]; /* 调试串口(USART1)接收缓冲 */

/* OTA 数据阶段的循环 DMA */
#define ESP_DMA_SIZE   2048u
#define ESP_DMA_MASK   (ESP_DMA_SIZE - 1u)

static uint8_t  ESP_DMA_Buf[ESP_DMA_SIZE];
static uint16_t dma_tail = 0;
static uint8_t  dma_on   = 0;

/* ============================================================================
 *  一、GPIO 与复位
 * ========================================================================== */

void ESP8266_GPIO_Init(void)//初始化代码
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOB, ESP8266_IO_Pin | ESP8266_RST_Pin, GPIO_PIN_SET);

    GPIO_InitStruct.Pin   = ESP8266_IO_Pin | ESP8266_RST_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* 硬复位：RST 拉低 100ms，再等 1.5 秒让模块启动 */
void ESP8266_Reset(void)
{
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(ESP8266_RST_GPIO_Port, ESP8266_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(1500);//要给WiFi足够的时间
}

/* 两个串口都用"空闲中断"方式开接收：调试口收 "OTA" 指令，ESP 口收 AT 回复 */
void UART_StartReceive(void)//开启监听
{
	  HAL_UARTEx_ReceiveToIdle_IT(&huart1, Debug_RxBuff, UART_RX_BUFF_LEN);//接收的是串口1，可用于在本地串口调试的时候用串口1发送指令
    HAL_UARTEx_ReceiveToIdle_IT(&huart3, it_buf, UART_RX_BUFF_LEN);//接收的是串口3，若只是简单指令而非固件传输，则用IT中断接收即可
}

/* ============================================================================
 *  二、接收中断
 * ========================================================================== */

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	  /* ---------- 调试串口：收到 "OTA" 三个字母 → 写升级请求标志 → 重启进 Bootloader ---------- */
    if (huart->Instance == USART1)
    {
        if (Size > UART_RX_BUFF_LEN - 1u) Size = UART_RX_BUFF_LEN - 1u;//保证串口缓冲区长度不超过最大接收缓冲长度
        Debug_RxBuff[Size] = 0;//设置串口接收缓冲区最后一位为\0，防止溢出

		  	if (strstr((char *)Debug_RxBuff, "OTA") != NULL)//检测到串口1中有本地发送的指令"OTA"
        {
            uint32_t flag = 0x5AA5A5A5u;//写入boot工程可识别的升级标志，让boot工程重新进入固件接收模式
            Internal_FLASH_Erase(0x0800FC00u, 16);//擦除原本存放升级标志的区域
            Internal_FLASH_Write(0x0800FC00u, (uint8_t *)&flag, 16);//写入该升级标志
            NVIC_SystemReset();//复位
        }

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, Debug_RxBuff, UART_RX_BUFF_LEN);//走到这里，说明没有识别成功，继续监听
        return;
    }

    /* ---------- ESP 串口：把这一段追加到累积缓冲（AT 回复常常分几段到达） ---------- */
    if (huart->Instance == USART3)//讲it_buf缓冲区中的数据拼接到ESP8266_RxBuff里面
    {
        if ((uint32_t)rx_len + Size < sizeof(ESP8266_RxBuff))//检测是否超出ESP8266_RxBuff最大长度
        {
            memcpy(&ESP8266_RxBuff[rx_len], it_buf, Size);//将it_buf内的数据复制到ESP8266_RxBuff中
            rx_len += Size;//更新偏移量，偏移量为当前已经传输的数据量
            ESP8266_RxBuff[rx_len] = 0;//末尾补上\0，防止溢出
        }
        ESP_RX_Complete = 1;//置1，告诉程序可以开始解析寻找特定指令了

        /* AT 阶段由这里重新开接收；OTA 阶段走 DMA，这里不能碰 */
        if (is_network_ready == 0)//检测标志位为0，说明没有开启固件接收，继续用IT中断接收
        {
            HAL_UARTEx_ReceiveToIdle_IT(&huart3, it_buf, UART_RX_BUFF_LEN);
        }
    }
}

/* 串口出错（最常见是 ORE 溢出）：清标志后把接收重新打开 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)//检测到出现溢出错误
{
    if (huart->Instance == USART3)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);//清除标志，保证串口继续运行

        if (is_network_ready == 0)
        {
            HAL_UARTEx_ReceiveToIdle_IT(&huart3, it_buf, UART_RX_BUFF_LEN);//依旧IT中断接收
        }
    }
}

/* ============================================================================
 *  三、AT 指令
 * ========================================================================== */

/* 发一条指令（自动补 \r\n），等模块回 OK。回复是分段到达的，所以这里边等边看累积缓冲，只要出现 OK 就算成功。 */
static uint8_t at_cmd(const char *cmd, uint32_t timeout_ms)
{
    char     buf[128];
    uint32_t t0;

    snprintf(buf, sizeof(buf), "%s\r\n", cmd);//格式化字符串，在末尾加上\r\n

    rx_len = 0;
    memset(ESP8266_RxBuff, 0, sizeof(ESP8266_RxBuff));//清除缓冲区

	  HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)strlen(buf), 1000);//狠狠地发送

    t0 = HAL_GetTick();//记录当前时间
    while (HAL_GetTick() - t0 < timeout_ms)//等待后，开始识别WiFi模块发回来的数据
    {
        if (strstr((char *)ESP8266_RxBuff, "OK")    != NULL) return 1;//表示成功，跳转
        if (strstr((char *)ESP8266_RxBuff, "ERROR") != NULL) return 0;//表示失败，返回
        if (strstr((char *)ESP8266_RxBuff, "FAIL")  != NULL) return 0;//表示失败，返回
        HAL_Delay(5);
    }
    return 0;//到了这里，表示wifi模块什么都数据都没有发，也是返回
}

/* 发数据（不等待、不解析），用于 OTA 阶段回 ACK/READY */
void ESP8266_SendString(char *str)
{
	HAL_UART_Transmit(&huart3, (uint8_t *)str, (uint16_t)strlen(str), 1000);//单向发送，不需要考虑wifi模块的回信
}

/* AT 测试 */
uint8_t ESP8266_Test(void)
{
    return at_cmd("AT", 500);
}

uint8_t ESP8266_TryExitTransparent(void)
{
    HAL_Delay(1100);
    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3, 1000);//发 "+++" 退出透传模式
    HAL_Delay(1100);//记得等待一秒，保证彻底退出

    return at_cmd("AT", 500);//再发一次AT
}

/* 确保模块能对话：先直接问，不行再 "+++"，再不行才硬复位。模块从上电起就一直跑着，所以绝大多数时候第一条 AT 就能过，不需要复位。 */
uint8_t ESP8266_Ready(void)
{
    static uint8_t inited = 0;

    if (inited == 0)
    {
        ESP8266_GPIO_Init();//初始化引脚
        inited = 1;
        HAL_Delay(1000);//给模块 1 秒启动 
    }

    UART_StartReceive();//开启串口接收             

    if (ESP8266_Test()) return 1; //试探模块是否存活。如果回了 AT，说明没卡在透传，直接放行

    printf("[ESP] 模块没回 AT，试 \"+++\" 退出透传\r\n");
    if (ESP8266_TryExitTransparent()) return 1;//发送+++后再发一次AT，再次检测

    printf("[ESP] 还不行，硬复位模块等 3 秒\r\n");
    ESP8266_Reset();//直接硬复位
    HAL_Delay(1500);
    return ESP8266_Test();
}

/* 工作模式：STA */
uint8_t ESP8266_SetMode(ENUM_Net_ModeTypeDef mode)
{
    char cmd[32];

    snprintf(cmd, sizeof(cmd), "AT+CWMODE=%d", (mode == AP) ? 2 : ((mode == STA_AP) ? 3 : 1));
	  //若模式为AP，输入模式2，若不为AP，再比较一下是不是STA_AP,若是，输入模式3，若不是，则说明函数传入的参数为STA，输入模式1
    return at_cmd(cmd, 2000);
}

/* 连接模式：单连接 */
uint8_t ESP8266_SetConnMode(ENUM_ConnModeTypeDef mode)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPMUX=%d", (mode == MULTI_CONN) ? 1 : 0);
    return at_cmd(cmd, 2000);
}

/* 连热点。连不上再试一次；如果模块其实早就连着（回 GOT IP），也算成功。 */
uint8_t ESP8266_ConnectWiFi(const char *ssid, const char *password)
{
    char cmd[128];
    uint8_t i;

    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, password);

    for (i = 0; i < 2; i++)
    {
        if (at_cmd(cmd, 25000)) return 1;
			if (strstr((char *)ESP8266_RxBuff, "GOT IP") != NULL) return 1;//到了这里，说明没有成功，去解析WiFi模块发送的数据有没有"GOT IP"，有的话说明已经连上了，也视为成功
        HAL_Delay(1000);
    }
    return 0;
}

/* 连 TCP 服务器 */
uint8_t ESP8266_ConnectTCPServer(const char *ip, uint16_t port, uint8_t conn_id)
{
    char cmd[128];
    at_cmd("AT+CIPCLOSE", 1000);//先关掉可能残留的连接

    if (conn_id == 0)
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", ip, port);
    else
        snprintf(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"TCP\",\"%s\",%d", conn_id, ip, port);

    return at_cmd(cmd, 15000);//给连接服务器多一点等待时间
}

/* 透传模式开关 */
uint8_t ESP8266_SetTransparentMode(ENUM_TransparentModeTypeDef mode)
{
    char cmd[32];

    snprintf(cmd, sizeof(cmd), "AT+CIPMODE=%d", (mode == TRANSPARENT_ON) ? 1 : 0);
    return at_cmd(cmd, 2000);
}

/* 进入发送数据模式 */
uint8_t ESP8266_SetSendMode(void)
{
    return at_cmd("AT+CIPSEND", 3000);
}

/* ============================================================================
 *  四、OTA 数据阶段：循环 DMA 接收
 *
 *  DMA 在后台一直往 ESP_DMA_Buf 这个 2KB 圆圈里搬数据，永不停止；
 *  主循环用"读指针 dma_tail"追上它即可，全程不需要重开接收。
 * ========================================================================== */

static uint16_t dma_head(void)
{
    return (uint16_t)(ESP_DMA_SIZE - (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart3_rx));
	  //用缓冲总长度减去剩余量，得出硬件当前写到的绝对位置
}

/* 切到循环 DMA 接收，开始传输固件 */
void ESP_DMA_RxStart(void)
{
    (void)HAL_UART_AbortReceive(&huart3);//关闭IT接收，准备使能DMA接收
    hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;//设置为循环模式
    (void)HAL_DMA_Init(&hdma_usart3_rx);//将刚才修改的循环模式配置，重新写入底层DMA寄存器
    (void)HAL_DMA_Start(&hdma_usart3_rx,//启动DMA硬件通道，开始自动搬运数据
                        (uint32_t)&huart3.Instance->DR,//要求串口将接收的数据放在DR寄存器中
                        (uint32_t)ESP_DMA_Buf, ESP_DMA_SIZE);//放在ESP_DMA_Buf这个大型缓冲区中
    SET_BIT(huart3.Instance->CR3, USART_CR3_DMAR);//让串口把数据交给 DMA

    dma_tail = 0;//偏离量归零，开始从缓冲区开头取数据
    dma_on   = 1;//表示DMA模式已激活
}

/* 停掉 DMA */
void ESP_DMA_RxStop(void)
{
    if (dma_on == 0) return;//表示DMA模式未激活，不用停了

    dma_on = 0;//表示DMA模式已关闭
    CLEAR_BIT(huart3.Instance->CR3, USART_CR3_DMAR);//把DMAR请了，防止出现总线错误
    (void)HAL_DMA_Abort(&hdma_usart3_rx);//关闭DMA控制器
}

/* 缓冲区里有多少字节还没取 */
uint16_t ESP_DMA_RxCount(void)
{
    return (uint16_t)((dma_head() - dma_tail) & ESP_DMA_MASK);//计算读取进度和写入进度的差值，总而得知还剩多少字节没取
}

/* 取走最多 max 字节，返回取到的数量 */
uint16_t ESP_DMA_RxRead(uint8_t *dst, uint16_t max)
{
    uint16_t n = ESP_DMA_RxCount();//获取剩余数据量
    uint16_t i;

    if (n > max) n = max;//设定取走的最多数量

    for (i = 0; i < n; i++)
    {
        dst[i]   = ESP_DMA_Buf[dma_tail];//缓冲区中数据一个个取到dst数组中
        dma_tail = (uint16_t)((dma_tail + 1u) & ESP_DMA_MASK);//不断更新索引，取一个往前挪一位
    }
    return n;
}

/* 丢掉缓冲里残留的数据（每轮开始前清一次） */
void ESP_DMA_RxFlush(void)
{
    dma_tail = dma_head();//让读取进度和写入进度的偏离量相等，视为没有要取的剩余字节
}