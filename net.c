#include "esp8266.h"
#include "usart.h"
#include "flash.h"
#include "net.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---------------- 存储布局 ---------------- */
#define STAGE_ADDR      0x00010000u          /* W25Q64 暂存区 */
#define STAGE_SECTORS   12u                  /* 12 个扇区 = 48KB */
#define FLAG_ADDR       0x0800FC00u          /* 内部 Flash 升级标志页 */
#define PKG_HEAD_LEN    12u                  /* "OTA1" + 长度 + CRC */
#define CHUNK_MAX       512u                 /* 一包最多 512 字节 */
#define APP_MAX_SIZE    (40u * 1024u)        /* 固件最大 40KB */

/* ---------------- 变量 ---------------- */
volatile uint8_t is_network_ready = 0;       /* 1 = 可以收 OTA 数据了（main 里会看） */

static uint32_t staged  = 0;                 /* 已写进 W25Q64 的字节数（含包头） */
static uint32_t ota_len = 0;                 /* 包头里的固件长度 */
static uint32_t ota_crc = 0;                 /* 包头里的 CRC32 */
static uint8_t  head_ok = 0;                 /* 包头解析成功 */
static uint8_t  erased  = 0;                 /* 暂存区已擦过（续传时不能再擦） */

static uint8_t  chunk[CHUNK_MAX];            /* 当前这一包的数据 */
static uint16_t need      = 0;               /* 这一包应该有多少字节 */
static uint8_t  waiting   = 0;               /* 1 = 正在等这一包的数据 */
static uint32_t wait_tick = 0;               /* 开始等这一包的时刻 */
static uint32_t idle_tick = 0;               /* 最后一次收到数据的时刻 */

static char     line[64];                    /* 命令行拼装缓冲 */
static uint8_t  line_len = 0;

/* ============================================================================
 *  一、小工具
 * ========================================================================== */

/* 给电脑回一句话（走 ESP8266 透传） */
static void reply(const char *fmt, ...)
{
    char buf[80];
    va_list ap;//定义一个变参列表指针

    va_start(ap, fmt);// 初始化指针，让ap指向fmt之后的第一个变长参数
    vsnprintf(buf, sizeof(buf), fmt, ap);//对传入的fmt参数进行格式化
    va_end(ap);//清理列表，防止内存泄露

	ESP8266_SendString(buf);//将处理好的的字符串发送给电脑
}

/* 擦除整个暂存区 */
static void stage_erase(void)
{
    uint8_t i;

    printf("[NET] 擦除暂存区...\r\n");
    for (i = 0; i < STAGE_SECTORS; i++)
    {
        SPI_FLASH_SectorErase(STAGE_ADDR + (uint32_t)i * 4096u);//一个扇区就是4KB，擦一次就是一个扇区
    }
    erased = 1;//表示暂存区已经擦干净了
}

/* 算 W25Q64 里一段数据的 CRC32 */
static uint32_t crc32_spi(uint32_t addr, uint32_t len)
{
    static uint8_t buf[128];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t n, i;
    uint8_t  k;

    while (len)
    {
        n = (len > sizeof(buf)) ? sizeof(buf) : len;//若要算的数据长度大于数组长度，则先分批读取
        SPI_FLASH_ReadBuffer(addr, buf, n);//从外部flash中读取数据

        for (i = 0; i < n; i++)
        {
            crc ^= buf[i];
            for (k = 0; k < 8; k++)
            {
                crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);//经典多项式除法
            }
        }
        addr += n;//更新偏移量
        len  -= n;//更新剩余量
    }
    return crc ^ 0xFFFFFFFFu;
}

/* 从 "CHUNK 1024 512" 里取出两个数字，成功返回 1 */
static uint8_t parse_two(const char *s, uint32_t *a, uint32_t *b)
{
    uint8_t got = 0;

    while (*s && *s != ' ') s++;//跳过命令字CHUNK,直到遇到空格
    while (*s == ' ') s++;//遇到空格也跳过

    *a = 0;
    while (*s >= '0' && *s <= '9') { *a = (*a * 10u) + (uint32_t)(*s++ - '0'); got = 1; }
	  //如果检测到了数字，将找到的数字传给*a，同时减去'0'，保证ASCII码就是真正的数值形态，同时每传入一个数字a就在加上数字乘十倍，从而实现数字的排列组成
    if (got == 0) return 0;//没有找到数字，返回
    while (*s == ' ') s++;//再次跳过空格，寻找下一个数字

    *b = 0;
    got = 0;//got再次置0，要求要找到两个数字
    while (*s >= '0' && *s <= '9') { *b = (*b * 10u) + (uint32_t)(*s++ - '0'); got = 1; }//同上
    return got;
}

/* ============================================================================
 *  二、收满之后的处理：解析包头 → 校验 CRC → 写标志 → 复位
 * ========================================================================== */

static void ota_check(void)
{
    uint8_t  head[PKG_HEAD_LEN];
    uint32_t total;

    /* 1. 够 12 字节了，解析一次包头 */
    if ((head_ok == 0) && (staged >= PKG_HEAD_LEN))//表示没有检查过，同时搬运量也达到包头长度了
    {
        SPI_FLASH_ReadBuffer(STAGE_ADDR, head, PKG_HEAD_LEN);//从外部flash中取出

        if (memcmp(head, "OTA1", 4) != 0)
        {
            printf("[NET] 包头不对，重新开始\r\n");
					  reply("ERR HEAD\r\n");
            stage_erase();//擦除，准备下一次接收
            staged = 0;
            reply("READY 0\r\n");
            return;
        }

        memcpy(&ota_len, head + 4, 4);//取包头第五到八个字节，该数据存储着固件长度的信息
        memcpy(&ota_crc, head + 8, 4);//取包头第九到十二个字节，该数据存储着固件特有的crc

        if ((ota_len == 0) || (ota_len > APP_MAX_SIZE))//若长度为0，或者长度超出APP区容量长度，返回
        {
            printf("[NET] 固件长度异常\r\n");
            reply("ERR LEN\r\n");
            stage_erase();
            staged = 0;
            reply("READY 0\r\n");
            return;
        }

        head_ok = 1;//表示已经检查过了
        printf("[NET] 包头: %lu 字节, CRC 0x%08lX\r\n",
               (unsigned long)ota_len, (unsigned long)ota_crc);
    }

    if (head_ok == 0) return;//到这里，如果head_ok还是0表示还没检查，返回

    /* 2. 还没收满就继续等 */
    total = PKG_HEAD_LEN + ota_len;//计算应该传输的总数据量，等于固件本身长度和包头大小的总和
    if (staged < total) return;//如果已传输数据量没到total，说明还没传完，返回

    /* 3. 收满了：先校验 W25Q64 里的数据，再决定要不要写标志 */
    printf("[NET] 收满了，校验 CRC...\r\n");
    if (crc32_spi(STAGE_ADDR + PKG_HEAD_LEN, ota_len) != ota_crc)//传完后，计算外部falsh内固件的crc码，比检查是否和要传输固件的rcr一致
    {
        printf("[NET] CRC 不对，重新开始\r\n");
        reply("ERR CRC\r\n");
        stage_erase();
        staged  = 0;
        head_ok = 0;
        reply("READY 0\r\n");
        return;
    }

    /* 4. 校验通过：写升级标志，重启交给 Bootloader 搬运 */
    printf("[NET] CRC 通过，写升级标志并重启\r\n");
    reply("OK FLASHING\r\n");
    {
        uint32_t flag[4];

        flag[0] = 0xAABB1122u;      // 魔数，和 Bootloader 一致 */
        flag[1] = ota_len;          // 固件长度 */
        flag[2] = ota_crc;          // CRC32 */
        flag[3] = PKG_HEAD_LEN;     // 数据在 W25Q64 里的偏移 */

        Internal_FLASH_Erase(FLAG_ADDR, 16);//擦除内部flash中存放升级标志的地方
        Internal_FLASH_Write(FLAG_ADDR, (uint8_t *)flag, 16);//写入升级标志
    }

    HAL_Delay(200);
    NVIC_SystemReset();
}

/* ============================================================================
 *  三、处理电脑发来的一行命令
 * ========================================================================== */

static void handle_line(char *s)
{
    uint32_t off, len;

    if (strncmp(s, "CHUNK ", 6) == 0)//检测开头是不是CHUNK这六个字
    {
        if (parse_two(s, &off, &len) == 0) { reply("ERR CMD\r\n"); return; }
        //开始从命令中提取数字，如果没有提取到数字，返回
        if ((len == 0) || (len > CHUNK_MAX))//如果len为0或者超出了最大单次传输限制，返回
        {
            reply("ERR CMD\r\n");
            return;
        }
        if (off != staged)//如果电脑偏移量和板子接收量不一致，返回
        {
            printf("[NET] 偏移不符: 电脑 %lu, 板子 %lu\r\n",
                   (unsigned long)off, (unsigned long)staged);
            reply("ERR OFFSET %lu\r\n", (unsigned long)staged);
            return;
        }

        need      = (uint16_t)len;
        wait_tick = HAL_GetTick();//记录当前时间，用于延时等待
        waiting   = 1;//表示程序开始接收数据
        return;
    }

    if (s[0] != 0) printf("[NET] 收到: %s\r\n", s);//不是协议命令的文本（模块状态、乱码等）打出来，方便排错 
}

/* ============================================================================
 *  四、建立链路（main 里调用；失败就返回，main 会隔几秒再试一次）
 * ========================================================================== */

void ESP8266_Task(void)
{
    printf("[NET] 连接网络...\r\n");

	  ESP_DMA_RxStop();//先切回IT模式，用于向wifi模块发送指令，等后面开始传输固件再改成DMA模式

    if (!ESP8266_Ready())                  { printf("[NET] 模块无响应\r\n");     return; }//检测模块是否正常
    if (!ESP8266_SetMode(STA))             { printf("[NET] 设置 STA 失败\r\n");  return; }//设置STA接收模式
    if (!ESP8266_SetConnMode(SINGLE_CONN)) { printf("[NET] 设置单连接失败\r\n"); return; }//设置单连接模式

    if (!ESP8266_ConnectWiFi(macUser_ESP8266_ApSsid, macUser_ESP8266_ApPwd))//连接wifi
    {
        printf("[NET] 连 Wi-Fi 失败\r\n");
        return;
    }
    printf("[NET] Wi-Fi 已连接\r\n");

    if (!ESP8266_ConnectTCPServer(macUser_ESP8266_TcpServer_IP,
                                  macUser_ESP8266_TcpServer_Port, 0))//连接tcp服务器
    {
        printf("[NET] 连服务器失败（电脑上的 send_ota.bat 在运行吗？）\r\n");
        return;
    }
    printf("[NET] 服务器已连接\r\n");

    if (!ESP8266_SetTransparentMode(TRANSPARENT_ON)) { printf("[NET] 设置透传失败\r\n"); return; }//设置为透传模式
    if (!ESP8266_SetSendMode())                      { printf("[NET] 进入发送模式失败\r\n"); return; }//让wifi模块进入发送模式

    if (!erased) stage_erase();//续传时不能擦，否则前面收的全没了

    ESP_DMA_RxStart(); //开启DMA中断，准备接收固件
    ESP_DMA_RxFlush();//清除接收缓冲区中的垃圾数据

    reply("READY %lu\r\n", (unsigned long)staged);//向服务器发送READY，表示自己已经进入状态了
    printf("[NET] 就绪，从第 %lu 字节开始收\r\n", (unsigned long)staged);

    line_len  = 0;// 命令行缓冲长度归零
    waiting   = 0;//表示当前还不准备给板子发送固件
    idle_tick = HAL_GetTick();//记录当前时间，用于后面的断流检测
    is_network_ready = 1;//表示当前已经可以开始远程接收固件了
}

/* ============================================================================
 *  五、收数据（main 的主循环里反复调用）
 * ========================================================================== */

void ESP8266_OTA_ReceiveTask(void)
{
    uint32_t now;
    uint8_t  b;

    if (is_network_ready == 0) return;//说明网络没连好，返回
    now = HAL_GetTick();//记录时间

    /* ---------- 数据阶段：等这一包到齐 ---------- */
    if (waiting)
    {
        if (ESP_DMA_RxCount() >= need)//查看DMA环形缓冲区中的数据是否大于单次传输的数据量了
        {
            ESP_DMA_RxRead(chunk, need);//如果大于了，就将其装成一包，放在chunk数组里面开始分包传输

            SPI_FLASH_WriteBuffer(STAGE_ADDR + staged, chunk, need);//传输进外部flash里面，已经传输的数量量此刻等于偏移量
            staged   += need;//更新偏移量
            waiting   = 0;//表示数据接收完毕
            idle_tick = now;//更新时间

					  reply("ACK %lu\r\n", (unsigned long)staged);//向服务器发送信息，并告诉服务器已经传输的数据量
            ota_check(); //检查有没有传完，如果没有就继续传
        }
        else if ((now - wait_tick) > 1500)//如果等了1.5秒还是没有传完，说明本次传输失败
        {
            printf("[NET] 这一包没收齐，重新连\r\n");
            is_network_ready = 0; // main 看到它变 0 就会重连，再次尝试传输
        }
        return;
    }

    /* ---------- 命令阶段：一个字节一个字节拼成一行 ---------- */
    while (ESP_DMA_RxCount() > 0)//到了这里，说明waiting是0，没有进入数据传输模式，正处于命令阶段。只要环形缓冲区中还有数据就继续循环
    {
        if (ESP_DMA_RxRead(&b, 1) != 1) break;
        idle_tick = now;//更新时间

        if (b == '\n')//说明读完一遍了
        {
            line[line_len] = 0;//防止溢出
            if (line_len > 0) handle_line(line);//如果缓冲区中有数据，就调用函数，处理命令
            line_len = 0;//清除缓冲区

            if (waiting) break;          /* 刚收到 CHUNK，转数据阶段 */
        }
        else if (b != '\r')//如果收到的不是换行符也不是回车符
        {
            if (line_len < (sizeof(line) - 1u)) line[line_len++] = (char)b;//从dma缓冲区中一个一个读取数据，拼接到line数组里面
            else                                line_len = 0;//超出最大限制了，直接清除
        }
    }

    if ((now - idle_tick) > 5000)//5 秒没有任何命令：说明链路已经不通了
    {
        printf("[NET] 长时间没有命令，重新连\r\n");
        is_network_ready = 0;
    }
}
