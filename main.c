#include "main.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"
#include <stdio.h>
#include "net.h"
#include "flash.h"
#define OTA_SOURCE_MSG   "本地串口升级成功"
#define OTA_MSG_PERIOD   2000 //设置打印间隔
#define DEMO_NO_NET      0
extern volatile uint8_t is_network_ready;
void SystemClock_Config(void);

/* printf 重定向到 USART1 */
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}

int main(void)
{
  uint32_t led_tick;
  uint32_t msg_tick;
#if (DEMO_NO_NET == 0)
  uint32_t last_net_try;
#endif

  SCB->VTOR = 0x08005000;//让中断向量的起始位置指向APP工程的初始位置
  HAL_Init();
  __enable_irq();//开启总中断
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  printf("\r\n[APP] 启动\r\n");

  /* ---------- 6. 连接 Wi-Fi 和 TCP（演示版跳过） ---------- */
#if (DEMO_NO_NET == 0)
  ESP8266_Task();//开始启动网络连接，尝试连接wifi和tcp
  last_net_try = HAL_GetTick();//用于更新时间
#else
  printf("[APP] 演示版：不连接服务器\r\n");
#endif

  /* ---------- 7. 主循环 ---------- */
  led_tick = HAL_GetTick();
  msg_tick = HAL_GetTick();   /* 【新增 2026-11-10】 */

  while (1)
  {
#if (DEMO_NO_NET == 0)
    if ((is_network_ready == 0) && (HAL_GetTick() - last_net_try > 3000))//启动网络连接后，如果超过三秒还没有成功连接wifi则重试
    {
      last_net_try = HAL_GetTick();//更新时间
      printf("[APP] 网络未就绪，重新连接...\r\n");
      ESP8266_Task();//再次试图连接
    }

    ESP8266_OTA_ReceiveTask();//处理 Wi-Fi 收到的升级数据 
#endif

    if (HAL_GetTick() - msg_tick >= OTA_MSG_PERIOD)//用于在串口中演示，每过两秒打印一次
    {
      msg_tick = HAL_GetTick();//更新时间
      printf("\r\n<<< " OTA_SOURCE_MSG " >>>\r\n");
    }

    if (HAL_GetTick() - led_tick >= 500)//每过半秒翻转一次红绿灯的电平
    {
      led_tick = HAL_GetTick();//更新时间
      HAL_GPIO_TogglePin(GPIOA, RED_Pin);
      HAL_GPIO_TogglePin(GPIOA, GREEN_Pin);
    }

    HAL_Delay(1);//重要，给中断处理时间，否则很容易出现死循环
  }
}

/**
  * @brief 系统时钟：8MHz 晶振 × 9 = 72MHz
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* 从 Bootloader 跳过来时，系统时钟已经是 PLL 锁定的 72MHz。
	   这里直接继承，不要去关/开 HSE —— STM32F1 的 HAL_RCC_OscConfig() 在"PLL 正在当系统时钟"时会拒绝重新配置，直接把 APP 送进 Error_Handler。 */
  if ((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL)
  {
      SystemCoreClock = 72000000U;//主频统一为72MHZ
      HAL_InitTick(TICK_INT_PRIORITY);
		  // HAL_Init 时默认按 8MHz 配了滴答定时器。这里必须重装 SysTick 到 72MHz，否则所有 HAL_Delay 和网络超时都会快 9 倍，AT 指令全等不到回复。
      return;//返回，不要到接触到后面的初始化代码
  }

  /* 下面这条路径只有"没用 Bootloader、DAP 直接烧录 APP"时才会走到 */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;      /* 8MHz × 9 = 72MHz */

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
