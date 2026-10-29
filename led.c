#include "led.h"
#include "gpio.h"
void RED_OFF(void)
{
	HAL_GPIO_WritePin(GPIOA,RED_Pin,GPIO_PIN_SET);
}

void RED_ON(void)
{
	HAL_GPIO_WritePin(GPIOA,RED_Pin,GPIO_PIN_RESET);
}

void BLUE_OFF(void)
{
	HAL_GPIO_WritePin(GPIOA,BLUE_Pin,GPIO_PIN_SET);
}

void BLUE_ON(void)
{
	HAL_GPIO_WritePin(GPIOA,BLUE_Pin,GPIO_PIN_RESET);
}

void GREEN_OFF(void)
{
	HAL_GPIO_WritePin(GPIOA,GREEN_Pin,GPIO_PIN_SET);
}

void GREEN_ON(void)
{
	HAL_GPIO_WritePin(GPIOA,GREEN_Pin,GPIO_PIN_RESET);
}

void LED_OFF(void)
{
	HAL_GPIO_WritePin(GPIOA,RED_Pin,GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA,BLUE_Pin,GPIO_PIN_SET);
	HAL_GPIO_WritePin(GPIOA,GREEN_Pin,GPIO_PIN_SET);
}

void LED_ON(void)
{
	HAL_GPIO_WritePin(GPIOA,RED_Pin,GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOA,BLUE_Pin,GPIO_PIN_RESET);
	HAL_GPIO_WritePin(GPIOA,GREEN_Pin,GPIO_PIN_RESET);
}