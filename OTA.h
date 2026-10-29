#ifndef __OTA_H
#define __OTA_H

#include "main.h"
#include <stdbool.h>
#define APP_START_ADDR  0x08005000   // APP 的起始地址
#define OTA_MAGIC_FLAG  0xAABB1122   // 升级成功魔数
#define FIRMWARE_SAVE_ADDR 0x00010000 // 固件在 W25Q64 里的存放地址
void OTA_Init(void);
void OTA_Task(void);
void JumpToApp(uint32_t app_addr);
void UpdateApp(void);
#endif