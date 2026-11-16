#ifndef __NET_H
#define __NET_H

#include "main.h"

/********************************** 用户需要设置的参数**********************************/
#define      macUser_ESP8266_ApSsid                       "OPPO Reno10 5G"         		//要连接的热点的名称
#define      macUser_ESP8266_ApPwd                        "12345678"           	//要连接的热点的密钥
#define      macUser_ESP8266_TcpServer_IP                 "192.168.202.139"     //要连接的服务器的 IP
#define      macUser_ESP8266_TcpServer_Port               6677               		//要连接的服务器的端口

// 定义外部 Flash 的存储起始地址（比如 0x00010000）
#define OTA_FIRMWARE_SAVE_ADDR  0x00010000 
// 定义标志位地址（跟 Bootloader 保持一致！）
#define OTA_FLAG_ADDR           0x0800FC00 
#define OTA_MAGIC_FLAG          0xAABB1122 
/************ 函数声明 ************/
void ESP8266_Task(void);
void ESP8266_OTA_ReceiveTask(void);
#endif /* __APP_ESP8266_H */
