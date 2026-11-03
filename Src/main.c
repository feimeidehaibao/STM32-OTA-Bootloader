/* ===========================================================================
 *  APP（跑在 0x08005000）—— 由 Bootloader 跳转进来
 *
 *  和普通工程唯一的区别，就是下面这 4 件事，其余代码照常写：
 *    1. main 的第一件事：把中断向量表挪到 0x08005000
 *    2. HAL_Init() 之后马上开总中断
 *    3. 时钟：Bootloader 已经配好 72MHz 就直接用，不要重复配置
 *    4. 工程里 IROM1 的起始地址必须是 0x08005000
 * =========================================================================== */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"
#include <stdio.h>

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* printf 重定向到 USART1 */
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}

int main(void)
{
  /* ---------- 1. 中断向量表指向 APP 自己 ---------- */
  /* 因为 APP 不是从 0x08000000 开始，硬件复位时用的还是 Bootloader 的表，
     必须把 VTOR 改成自己的，中断才会跳到 APP 的 Handler 里 */
  SCB->VTOR = 0x08005000;

  /* ---------- 2. HAL 初始化 ---------- */
  HAL_Init();

  /* ---------- 3. 打开总中断 ---------- */
  /* Bootloader 跳过来时，全局中断还是关着的（PRIMASK=1）。
     如果等到外设初始化完才开，这段时间 SysTick 不计数，HAL_GetTick()
     永远不增加，HAL_Delay() 和 HAL 里所有超时判断都会卡死。 */
  __enable_irq();

  /* ---------- 4. 时钟 ---------- */
  SystemClock_Config();

  /* ---------- 5. 外设初始化 ---------- */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();

  /* ---------- 6. 主循环：PA1 红灯 500ms 闪一次 ---------- */
  while (1)
  {
      HAL_GPIO_TogglePin(GPIOA, RED_Pin);
		HAL_GPIO_TogglePin(GPIOA, BLUE_Pin);
		HAL_GPIO_TogglePin(GPIOA, GREEN_Pin);
		  printf("\r\n OK,UPUP! \r\n");
      HAL_Delay(10000);
  }
}

/**
  * @brief 系统时钟：8MHz 晶振 × 9 = 72MHz
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* 如果是 Bootloader 跳过来的，系统时钟已经是 PLL 锁定的 72MHz。
     这里直接继承，不要去关/开 HSE —— 因为 STM32F1 的 HAL_RCC_OscConfig()
     在"PLL 正在当系统时钟"时会拒绝重新配置，直接返回 HAL_ERROR，
     APP 就会掉进 Error_Handler 死循环（这就是之前 OTA 后不亮灯的原因之一）。 */
  if ((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL)
  {
      SystemCoreClock = 72000000U;
      return;
  }

  /* 下面这条路径只有在"没用 Bootloader、DAP 直接烧录 APP"时才会走到 */
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
