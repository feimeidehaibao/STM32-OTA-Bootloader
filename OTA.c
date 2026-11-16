/* ===========================================================================
 *  OTA 升级 —— 串口收包 → W25Q64 暂存 → 内部 Flash
 *
 *  一、两种升级包都能收
 *
 *    【推荐】带包头的升级包（用 pack_ota.bat 生成）：
 *      +--------+--------+--------+----------------+
 *      | "OTA1" |  长度   | CRC32  |   app.bin 数据  |
 *      | 4 字节 | 4 字节  | 4 字节  |    N 字节       |
 *      +--------+--------+--------+----------------+
 *      长度、CRC 都是"小端"，CRC 只算数据部分。
 *      好处：能判断"收全了没有、传坏了没有"，校验不过绝不跳转。
 *
 *    【兼容】裸 bin（就是 fromelf 直接生成的 app.bin）：
 *      没有包头，整个收到的数据当固件用。能用，但没法比对 CRC，
 *      只做"栈顶/复位向量是否合法"的检查。
 *
 *  二、接收流程
 *      串口中断只做一件事：把字节丢进 2KB 环形缓冲。
 *      主循环负责把缓冲里的数据攒够 512 字节就往 W25Q64 写，
 *      这样主循环写 Flash 的几毫秒里新数据落在缓冲里，一个都不丢。
 *
 *  三、标志页（0x0800FC00）里存 4 个字
 *      [0] 魔数 0xAABB1122   [1] 固件长度
 *      [2] CRC32             [3] 数据在 W25Q64 里的偏移（0 或 12）
 */

#include "OTA.h"
#include "flash.h"
#include "usart.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>
#include "dma.h"

/* ---------------- 可调参数 ---------------- */
#define PKG_HEAD_LEN    12                     /* 包头长度 */
#define APP_MAX_SIZE    (40 * 1024)            /* APP 最大 40KB */
#define STAGE_SECTORS   12                     /* W25Q64 暂存区占 12 个扇区(48KB) */
#define RING_SIZE       2048                   /* 接收环形缓冲大小，必须是 2 的幂 */
#define RING_MASK       (RING_SIZE - 1)
#define WRITE_CHUNK     512                    /* 每次往 W25Q64 写 512 字节 */

/* ---------------- 全局变量 ---------------- */
static uint8_t  ring[RING_SIZE];               /* 环形接收缓冲 */
static volatile uint16_t ring_head = 0;        /* 写指针：只有中断在改 */
static volatile uint16_t ring_tail = 0;        /* 读指针：只有主循环在改 */

static uint8_t  rx_byte = 0;                   /* HAL 单字节接收用 */
static uint32_t staged  = 0;                   /* 已经写进 W25Q64 的字节数 */
static uint32_t rx_total = 0;                  /* 总共收到的字节数 */
static uint32_t last_rx_tick = 0;              /* 最后一次收到数据的时刻 */
static uint32_t lost = 0;                      /* 缓冲满时丢掉的字节数 */
#define RX_DMA_SIZE     2048                   //DMA 缓冲区大小，必须是 2 的幂
#define RX_DMA_MASK     (RX_DMA_SIZE - 1)
static uint8_t  rx_dma[RX_DMA_SIZE];           //DMA 一直在往这个"圆圈"里搬数据
static uint16_t rx_tail = 0;                   //主循环已经取到圆圈的哪个位置
static uint32_t uart_err = 0;                  //串口出错次数
static uint8_t  rx_enabled = 0;                //接收是否开着（1=开着）
static DMA_HandleTypeDef hdma_usart1_rx;       //USART1 接收用的 DMA 句柄


static void uart_rx_start(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();                       //打开 DMA1 时钟

    hdma_usart1_rx.Instance                 = DMA1_Channel5;          //F103 上 USART1_RX 固定是通道5
    hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;   //外设 -> 内存
    hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;       //外设地址不动
    hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;        //内存地址递增
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode                = DMA_CIRCULAR;           //★循环模式：搬到底自动回到开头
    hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&hdma_usart1_rx);

    //直接启动 DMA，目标是串口的数据寄存器(DR)，搬 RX_DMA_SIZE 个字节
    if (HAL_DMA_Start(&hdma_usart1_rx, (uint32_t)&huart1.Instance->DR, (uint32_t)rx_dma, RX_DMA_SIZE) != HAL_OK)
    {
        printf("[OTA] 警告：DMA 启动失败\r\n");      
    }

    SET_BIT(huart1.Instance->CR3, USART_CR3_DMAR);     //让串口把收到的数据交给 DMA

    rx_tail    = 0;
    rx_enabled = 1;
}

/* 擦 W25Q64 之前必须先停，不然擦除期间的数据会乱  */
static void uart_rx_stop(void)
{
    rx_enabled = 0;
    CLEAR_BIT(huart1.Instance->CR3, USART_CR3_DMAR);   //先断开串口和 DMA 的联系
    HAL_DMA_Abort(&hdma_usart1_rx);                    //再停 DMA
}




/* ===========================================================================
 *  CRC32识别部分
 * =========================================================================== */
 //指纹提取函数，读取bin整个文件，并算出4字节的CRC
static uint32_t crc32(uint32_t crc,uint8_t data)
{
	crc ^=data;
	for(int i=0;i<8;i++)
	{
		if(crc%2==1)
		{
			crc=(crc>>1)^0xEDB88320U;//若为奇数，就并上生成多项式，并右移一位
		}
		else
		{
			crc=(crc>>1);//若为偶数，仅右移一位
		}
	}
	return crc;
}
/* 算内部 Flash 上 len 字节的 CRC */
static uint32_t crc32_of_flash(uint32_t addr, uint32_t n)
{
    uint32_t crc=0xFFFFFFFFU;
	  for(int i=0;i<n;i++)
	{
		crc=crc32(crc,*(volatile uint8_t*)addr);
		addr++;
	}
	return crc ^0xFFFFFFFFU;
}

/* 算 W25Q64 上数据的 CRC（每次读 256 字节再算，省内存） */
static uint32_t crc32_of_spi(uint32_t addr, uint32_t len)
{
    static uint8_t tmp[256];
    uint32_t crc = 0xFFFFFFFFU;

    while (len)
    {
        uint32_t n = (len > sizeof(tmp)) ? sizeof(tmp) : len;
        SPI_FLASH_ReadBuffer(addr, tmp, n);
        for (uint32_t i = 0; i < n; i++) crc = crc32(crc, tmp[i]);
        addr += n;
        len  -= n;
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ===========================================================================
 *  数据接收部分，将串口中的bin文件写入flash中
 * =========================================================================== */


//清空flash暂存区：停接收 → 擦 W25Q64 → 所有计数归零 → 重新开接收

static void reset_staging(void)
{
	uart_rx_stop();     //先停 DMA 接收，再擦除
	for(uint8_t i=0;i<STAGE_SECTORS;i++)//擦除flash的暂存区
	{
	SPI_FLASH_SectorErase(FIRMWARE_SAVE_ADDR+(uint32_t)i*4096);
	}
	  ring_head = 0;
    ring_tail = 0;
    staged    = 0;
    rx_total  = 0;
    lost      = 0;
    last_rx_tick = HAL_GetTick();//记录时间
  uart_rx_start();   //用 DMA 启动接收
}


static void ring_push(uint8_t *src, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
    {
        uint32_t majiaqi = (ring_head + 1) & RING_MASK;   //head自增逻辑，同时保证不溢出数组
        if (majiaqi != ring_tail)
        {
            ring[ring_head] = src[i];
            ring_head = majiaqi;
            rx_total++;                                   //记录总接收字节数
        }
        else
        {
            lost++;                                       //环形缓冲满了，记录丢失的字节数
        }
    }
    if (n > 0) last_rx_tick = HAL_GetTick();
}

static void dma_to_ring(void)
{
    uint16_t head;
    uint16_t count;
    uint16_t n;

    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE))//检测是否出现串口错误溢出
    {
        __HAL_UART_CLEAR_OREFLAG(&huart1);//清除错误溢出标志，保证串口继续运行
        uart_err++;
    }

    head  = (uint16_t)(RX_DMA_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));//索引缓冲区中搬到了哪里
    count = (uint16_t)((head - rx_tail) & RX_DMA_MASK);//记录差值

    if (count == 0) return;

    if ((uint32_t)rx_tail + count <= RX_DMA_SIZE)
    {
        ring_push(&rx_dma[rx_tail], count);            //没跨过圆圈末尾，一次搬完
    }
    else
    {
        n = (uint16_t)(RX_DMA_SIZE - rx_tail);         //先搬到末尾
        ring_push(&rx_dma[rx_tail], n);
        ring_push(&rx_dma[0], (uint16_t)(count - n));  //再从开头接着搬
    }

    rx_tail = (uint16_t)((rx_tail + count) & RX_DMA_MASK);
    last_rx_tick = HAL_GetTick();                      //记录最后一次收到数据的时刻
}

//实时检测ring_head和ring_tail的差值
static uint16_t ring_count(void)
{
	return (uint16_t)((ring_head-ring_tail)&RING_MASK);
}

//缓冲里攒够一整块就写出去，差值大于等于512的时候就调用flash写入函数
static void drain_ring(void)
{
   while(ring_count()>=WRITE_CHUNK)
	 {
		 ring_write_to_flash(FIRMWARE_SAVE_ADDR+staged, WRITE_CHUNK);//写入的地址等于起始地址加上写入的字节量
	 }
}

static void  ring_write_to_flash(uint32_t majiaqi,uint16_t n)
{
	uint16_t shengyu=RING_SIZE-ring_tail;//计算tail到缓冲区末尾还有多少字节
	if(shengyu>=n)//如果剩余量大于写入量
	{
		SPI_FLASH_WriteBuffer(majiaqi, &ring[ring_tail], n);//直接狠狠写入
	}
	else//如果剩余量小于写入量
	{
		SPI_FLASH_WriteBuffer(majiaqi, &ring[ring_tail], shengyu);//先写入剩余量的数据
		SPI_FLASH_WriteBuffer(majiaqi+shengyu, &ring[0], n-shengyu);//再返回到缓冲区的开头，把剩下的数据写完
	}
	ring_tail=(ring_tail+n)&RING_MASK;//ring_tail同步写入的数据量，同时防止溢出
	staged+=n;//这个同步写入的数据量，用于计算应该在哪里开始写入数据
}


/* ===========================================================================
 *  进入 OTA 接收模式
 * =========================================================================== */
void OTA_Init(void)
{
    printf("擦除 W25Q64 暂存区(%d 个扇区)...\r\n", STAGE_SECTORS);
    reset_staging();
    printf("擦除完成，请发送 app_ota.bin 或 app.bin\r\n");
}

/* ===========================================================================
 *  检查函数，检测包头，检查传输过程是否出错，检查从串口传输进来的bin文件是否正常
 * =========================================================================== */
static void finish_package(void)
{
	uint8_t tou[PKG_HEAD_LEN];
	uint32_t len=0;
	uint32_t CRC_wai=0;
	if(lost >0)
	{
		printf("\r\n 出现丢失字节，升级失败 \r\n");
		return;
	}
	if(staged<=16)
	{
		printf("\r\n 数据传输过少，升级失败 \r\n");
		return;
	}
	SPI_FLASH_ReadBuffer(FIRMWARE_SAVE_ADDR,tou,PKG_HEAD_LEN);
	if(memcmp(tou,"OTA1",4)!=0)
	{
		printf("\r\n 包头不对，升级失败 \r\n");
		printf("   [诊断] staged=%u rx_total=%u lost=%u uart_err=%u 包头=",
		       (unsigned)staged,(unsigned)rx_total,(unsigned)lost,(unsigned)uart_err);
		for (int k = 0; k < 12; k++) printf("%02X ", tou[k]);
		printf("\r\n");
		return;
	}
	memcpy(&len,tou+4,4);
	memcpy(&CRC_wai,tou+8,4);
	if((len==0)||(len>APP_MAX_SIZE))
	{
		printf("\r\n 固件长度异常,长度为%u，升级失败 \r\n",(unsigned)len);
		return;
	}
	if(staged!=len+PKG_HEAD_LEN)
	{
		printf("\r\n 实际传输数据大小和固件大小不一致，升级失败，实际传输大小为%u，应传输%u \r\n",(unsigned)staged, (unsigned)(PKG_HEAD_LEN + len));
		printf("   [诊断] lost=%u  uart_err=%u  rx_total=%u  包头12字节=", (unsigned)lost, (unsigned)uart_err, (unsigned)rx_total);
		for (int k = 0; k < 12; k++) printf("%02X ", tou[k]);//将包头打印出来，若出现错误可直接诊断
		printf("\r\n");
		return;
	}
	uint32_t crc_ben=crc32_of_spi(FIRMWARE_SAVE_ADDR+PKG_HEAD_LEN,len);
	if(CRC_wai!=crc_ben)
	{
		printf("\r\n CRC不一样，固件自身的crc为0x%08X，传输进来的数据crc为0x%08X \r\n",(unsigned)CRC_wai,(unsigned)crc_ben);
		return;
	}
	printf("\r\n 已确认传输过程无错误，已传输%u字节 \r\n",(unsigned)len);
	
	uint32_t vec[2];
	SPI_FLASH_ReadBuffer(FIRMWARE_SAVE_ADDR + PKG_HEAD_LEN, (uint8_t *)vec, 8);//在外部flash中提取sp和pc信息
  uint32_t sp, pc;
    sp = vec[0];
    pc = vec[1];
    if (((sp & 0x2FFE0000U) != 0x20000000U) || (pc < APP_START_ADDR) || (pc >= 0x08010000U)) //检测sp和pc是否正常
			{
        printf("固件头不对: SP=0x%08X PC=0x%08X，可能没传完整\r\n", (unsigned)sp, (unsigned)pc);
        return;
    }
	uint32_t Flag[4];
  Flag[0]=OTA_MAGIC_FLAG;//写入魔数
	Flag[1]=len;
	Flag[2]=CRC_wai;
	Flag[3]=PKG_HEAD_LEN;
	 Internal_FLASH_Erase(OTA_FLAG_ADDR,16);//清除内部flash存放升级标志区域的数据
	 Internal_FLASH_Write(OTA_FLAG_ADDR,(uint8_t *)Flag,16);//写入升级标志
	printf("\r\n 升级标志已写入，开始软复位 \r\n");
		HAL_Delay(100);
		NVIC_SystemReset();
}

/* ===========================================================================
 *  主循环里反复调用：搬数据 + 判断"发完了没有"
 * =========================================================================== */
void OTA_Task(void)
{
    dma_to_ring();   //先把 DMA 收到的新数据搬进环形缓冲
    drain_ring();    //再把环形缓冲里的数据写进 W25Q64

    /* 2 秒没有新数据，就认为发完了 */
    if (rx_total == 0) return;
    if ((HAL_GetTick() - last_rx_tick) < 2000) return;

    /* 缓冲里还剩不到 512 字节的尾巴，一次性写完 */
    if (ring_count() > 0)
    {
        ring_write_to_flash(FIRMWARE_SAVE_ADDR + staged, ring_count());
    }

    finish_package();

    /* 走到这里说明这一包没通过：清空暂存区，等重新发 */
    reset_staging();

    printf("请重新发送固件\r\n");
}

/* ===========================================================================
 *  正式跳转到 APP，开始运行APP工程
 * =========================================================================== */
void JumpToApp(uint32_t app_addr)
{
    typedef void (*pFunction)(void);

    uint32_t sp = *(__IO uint32_t *)app_addr;        /* 第 1 个字：栈顶 */
    uint32_t pc = *(__IO uint32_t *)(app_addr + 4);  /* 第 2 个字：复位入口 */

    /* 1. 检查两个地址是否合理 */
    if (((sp & 0x2FFE0000U) != 0x20000000U) || (pc < app_addr) || (pc >= 0x08010000U))
    {
        printf("APP 区为空或非法: SP=0x%08X PC=0x%08X\r\n", (unsigned)sp, (unsigned)pc);
        return;
    }

    /* 2. 前 8KB 里如果大部分是 0xFF，说明这是一包没传完的固件，不跳 */
    {
        uint32_t empty = 0;
        for (uint32_t i = 0; i < 8192U; i++)
        {
            if (*(volatile uint8_t *)(app_addr + i) == 0xFF) empty++;//若出现空白字节，则计数
        }
        if (empty > (8192U * 7U / 10U))
        {
            printf("APP 区是半截镜像(0xFF 占 %u%%)，不跳转\r\n", (unsigned)(empty * 100U / 8192U));
					//如果空白占比大于70%，则判断传输进来的数据不合理，不像固件，拒绝跳转
            return;
        }
    }

    printf("跳转到 APP: SP=0x%08X PC=0x%08X\r\n", (unsigned)sp, (unsigned)pc);

    /* 3. 交权：关中断、关 SysTick、清 NVIC、向量表指过去、换栈 */
		uart_rx_stop();                        //停 DMA + 断开串口 DMA 请求
    CLEAR_BIT(SPI1->CR1, SPI_CR1_SPE);     //关掉 SPI1
    __disable_irq();//关闭总中断
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;//关闭计数器
    for (int i = 0; i < 8; i++) NVIC->ICER[i] = 0xFFFFFFFF;//关闭所有外部中断权限
    for (int i = 0; i < 8; i++) NVIC->ICPR[i] = 0xFFFFFFFF;//清除所有中断标志
    SCB->VTOR = app_addr;//将向量表偏移量设置为 APP 的基地址
    __DSB();
    __ISB();//数据/指令同步屏障

    __set_MSP(sp);//设置堆栈指针
    __DSB();
    __ISB();//再次同步

    ((pFunction)pc)();  //函数指针跳转到 APP 的复位处理程序
}

/* ===========================================================================
 *  把 W25Q64 里的固件搬到内部 Flash，实现内部升级
 * =========================================================================== */
void UpdateApp(void)
{
	/* 1. 先比较已经存入的升级标志是否正确，固件大小是否合理 */
	uint32_t flag[4];
	Internal_FLASH_Read(OTA_FLAG_ADDR, (uint8_t *)flag, 16);
	if(flag[0]!=OTA_MAGIC_FLAG)
	{
		printf("\r\n 升级标志不对	\r\n");
		return;
	}
	uint32_t len    = flag[1];
  uint32_t crc    = flag[2];
  uint32_t offset = flag[3];

  if ((len == 0) || (len > APP_MAX_SIZE)) 
		{
        printf("固件长度异常: %u\r\n", (unsigned)len);
        return;
    }
  if ((offset != 0) && (offset != PKG_HEAD_LEN))
		{
        printf("数据偏移异常: %u\r\n", (unsigned)offset);
        return;
    }
	printf(" \r\n 开始搬运，预计搬运%u个字节 \r\n",(unsigned)len);
	if(!Internal_FLASH_Erase(OTA_FLAG_ADDR, 16))
	{
		printf(" \r\n 升级标志擦除失败 \r\n");
		return;
	}
	if(!Internal_FLASH_Erase(APP_START_ADDR,(len+1023U)&~1023U))
	{
		printf(" \r\n APP工程区域清理失败 \r\n");
		return;
	}
	/* 2. 开始正式从外部flash搬运固件，并写入内部flash中 */
	static uint8_t majiaqi[256];//加上static，否则会把栈撑爆
	for(uint32_t i=0;i<len;i+=sizeof(majiaqi))
	{
		uint32_t pipi=((len-i)>sizeof(majiaqi)?sizeof(majiaqi):(len-i));//预计每次搬运256个字节，若剩余的数据量小于256个字节，就直接把剩余的数据量搬运完
		SPI_FLASH_ReadBuffer(FIRMWARE_SAVE_ADDR+offset+i,majiaqi,pipi);//每次搬运完成都会有指针偏移，偏移量为已搬运的数据量加上头文件长度
		if(!Internal_FLASH_Write(APP_START_ADDR+i,majiaqi,pipi))//每次搬运完成都会有指针偏移，偏移量为已搬运的数据量
		{
			printf("\r\n 写入内部flash失败 \r\n");
			return;
		}
	}
	/* 3. 对内部 Flash 再算一遍 CRC，和标志里存的比对 */
    {
        uint32_t crc_src   = crc32_of_spi(FIRMWARE_SAVE_ADDR + offset, len);
        uint32_t crc_flash = crc32_of_flash(APP_START_ADDR, len);

        printf("CRC: 包头=0x%08X  W25Q64=0x%08X  内部Flash=0x%08X\r\n",
               (unsigned)crc, (unsigned)crc_src, (unsigned)crc_flash);

        if (crc_flash != crc)
        {
            if (crc_src != crc) printf("=> W25Q64 里的数据就是坏的（Wi-Fi 传输丢包）\r\n");
            else                printf("=> W25Q64 好，是写内部 Flash 出的问题\r\n");
            printf("搬运后校验失败！\r\n");
            return;
        }
    }
    printf("搬运并校验通过\r\n");

    /* 4. 正式跳转 */
  HAL_Delay(100);
  JumpToApp(APP_START_ADDR);
}