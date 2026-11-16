#ifndef __ESP8266_H
#define __ESP8266_H

#include "main.h"

/******************************* ESP8266 数据类型定义 ***************************/

/**
  * @brief  ESP8266 网络工作模式枚举类型
  */
typedef enum{
    STA,        // 站点模式：作为客户端连热点
    AP,         // 接入点模式：自己当热点
    STA_AP      // 两种同时
} ENUM_Net_ModeTypeDef;

/**
  * @brief  ESP8266 连接模式枚举类型
  */
typedef enum {
    SINGLE_CONN  = 0,  // 单连接
    MULTI_CONN   = 1   // 多连接
} ENUM_ConnModeTypeDef;

/**
  * @brief  ESP8266 传输模式枚举类型
  */
typedef enum {
    TRANSPARENT_OFF = 0,  // 普通模式
    TRANSPARENT_ON  = 1   // 透传模式
} ENUM_TransparentModeTypeDef;

/******* ESP8266 控制引脚 *******/
#define ESP8266_IO_Pin       	GPIO_PIN_8
#define ESP8266_IO_GPIO_Port 	GPIOB
#define ESP8266_RST_Pin      	GPIO_PIN_9
#define ESP8266_RST_GPIO_Port GPIOB

/* 接收缓冲长度 */
#define UART_RX_BUFF_LEN 512

/************ 函数声明 ************/

/* 基础：引脚、复位、串口接收 */
void ESP8266_GPIO_Init(void);
void ESP8266_Reset(void);
void UART_StartReceive(void);

/* 发数据 / AT 指令 */
void    ESP8266_SendString(char *str);
uint8_t ESP8266_Test(void);
uint8_t ESP8266_Ready(void);
uint8_t ESP8266_TryExitTransparent(void);
uint8_t ESP8266_SetMode(ENUM_Net_ModeTypeDef mode);
uint8_t ESP8266_SetConnMode(ENUM_ConnModeTypeDef mode);
uint8_t ESP8266_ConnectWiFi(const char *ssid, const char *password);
uint8_t ESP8266_ConnectTCPServer(const char *ip, uint16_t port, uint8_t conn_id);
uint8_t ESP8266_SetTransparentMode(ENUM_TransparentModeTypeDef mode);
uint8_t ESP8266_SetSendMode(void);

/* OTA 数据阶段：循环 DMA 接收 */
void     ESP_DMA_RxStart(void);
void     ESP_DMA_RxStop(void);
uint16_t ESP_DMA_RxCount(void);
uint16_t ESP_DMA_RxRead(uint8_t *dst, uint16_t max);
void     ESP_DMA_RxFlush(void);

#endif /* __ESP8266_H */
