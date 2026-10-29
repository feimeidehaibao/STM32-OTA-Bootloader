#ifndef __FLASH_H
#define __FLASH_H

#include "main.h"
#include "spi.h"
#include <stdbool.h>
#define FLASH_CS_Pin            GPIO_PIN_4                      // SPI Flash片选引脚，PA4
#define FLASH_CS_GPIO_Port      GPIOA                           // 片选引脚所在GPIO端口

#define FLASH_CS_LOW()          HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET)  // 拉低片选，开始通信
#define FLASH_CS_HIGH()         HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)    // 拉高片选，结束通信

#define WFLASH_ID               0xEF4017                       // W25Q64芯片ID

#define W25Q64_SPI_FLASH_PAGESIZE       256                     // W25Q64页大小，单位字节
#define W25Q64_SPI_FLASH_SIZE           (W25Q64_SPI_FLASH_PAGESIZE * 8)  // W25Q64总大小，单位字节

#define W25Q64_MANUFACTDEVICEID_ADDRESS 0x000000                 // 芯片制造商及设备ID地址
#define W25Q64_DUMMY_BYTE               0xFF                      // 通信中的无效字节

#define WIP_FLAG                       0x01                      // 写入中标志，FLASH内部正忙于写入

/**************** 内部 STM32 Flash 操作定义 ****************/

#define STM32_FLASH_BASE    0x08000000
#define STM32_FLASH_END     0x0800FFFF
#define STM32_PAGE_SIZE     1024        // F103C8T6 的页大小为 1KB
#define OTA_FLAG_ADDR       0x0800FC00  // 最后一页的首地址，用来存标志位

// 内部 Flash 函数声明 (加了 Internal_ 前缀)
bool Internal_FLASH_Read(uint32_t readAddr, uint8_t *pBuffer, uint32_t numToRead);
bool Internal_FLASH_Write(uint32_t writeAddr, uint8_t *pBuffer, uint32_t numToWrite);
bool Internal_FLASH_Erase(uint32_t eraseAddr, uint32_t numToErase);
/********************************W25Q64命令定义*******************************/

#define W25Q64_WRITE_ENABLE                     0x06    // 写使能指令
#define W25Q64_WRITE_DISABLE                    0x04    // 写禁止指令
#define W25Q64_READ_STATUS_REGISTER_1           0x05    // 读状态寄存器1
#define W25Q64_READ_STATUS_REGISTER_2           0x35    // 读状态寄存器2
#define W25Q64_WRITE_STATUS_REGISTER            0x01    // 写状态寄存器
#define W25Q64_PAGE_PROGRAM                     0x02    // 页面编程指令
#define W25Q64_QUAD_PAGE_PROGRAM                0x32    // 四线页面编程
#define W25Q64_BLOCK_ERASE_64KB                 0xD8    // 擦除64KB块
#define W25Q64_BLOCK_ERASE_32KB                 0x52    // 擦除32KB块
#define W25Q64_SECTOR_ERASE_4KB                 0x20    // 擦除4KB扇区
#define W25Q64_CHIP_ERASE                       0xC7    // 全片擦除
#define W25Q64_ERASE_SUSPEND                    0x75    // 擦除暂停
#define W25Q64_ERASE_RESUME                     0x7A    // 擦除恢复
#define W25Q64_POWER_DOWN                       0xB9    // 进入掉电模式
#define W25Q64_HIGH_PERFORMANCE_MODE            0xA3    // 高性能模式
#define W25Q64_CONTINUOUS_READ_MODE_RESET       0xFF    // 连续读取模式复位
#define W25Q64_RELEASE_POWER_DOWN_HPM_DEVICE_ID 0xAB    // 释放掉电/高性能模式，并读取设备ID
#define W25Q64_MANUFACTURER_DEVICE_ID           0x90    // 读取制造商和设备ID
#define W25Q64_READ_UNIQUE_ID                   0x4B    // 读取唯一ID
#define W25Q64_JEDEC_ID                         0x9F    // 读取JEDEC ID
#define W25Q64_READ_DATA                        0x03    // 读取数据指令
#define W25Q64_FAST_READ                        0x0B    // 快速读取指令
#define W25Q64_FAST_READ_DUAL_OUTPUT            0x3B    // 双线快速读取输出
#define W25Q64_FAST_READ_DUAL_IO                0xBB    // 双线快速读取IO
#define W25Q64_FAST_READ_QUAD_OUTPUT            0x6B    // 四线快速读取输出
#define W25Q64_FAST_READ_QUAD_IO                0xEB    // 四线快速读取IO
#define W25Q64_OCTAL_WORD_READ_QUAD_IO          0xE3    // 八线字快速读取四线IO


/************ 函数声明 ************/
void FLASH_CS_GPIO_Init(void);
uint16_t SPI_FLASH_ReadDeviceID(void);
uint32_t SPI_FLASH_JEDEC_ID(void);
void SPI_FLASH_WriteEnable(void);
void SPI_FLASH_WaitForWriteEnd(void);
void SPI_FLASH_PowerDown(void);
void SPI_FLASH_WakeUp(void);
void SPI_FLASH_BulkErase(void);
void SPI_FLASH_SectorErase(uint32_t sector_addr);
void SPI_FLASH_ReadBuffer(uint32_t read_addr, uint8_t* buffer, uint32_t size);
void SPI_FLASH_WritePage(uint32_t write_addr, uint8_t* buffer, uint16_t size);
void SPI_FLASH_WriteBuffer(uint32_t write_addr, uint8_t* buffer, uint32_t size);

#endif /* __BSP_SPI_W25Q64_H */