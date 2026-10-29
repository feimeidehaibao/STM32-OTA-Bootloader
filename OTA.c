#include "OTA.h"
#include "flash.h"
#include "usart.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>



static uint8_t rx_byte = 0;              
static uint8_t ota_buffer[1024];         
static uint16_t ota_len = 0;             
static uint32_t write_flash_addr = FIRMWARE_SAVE_ADDR; 
static bool ota_started = false;         
static uint32_t last_rx_tick = 0;
static bool buffer_full = false; // 标志缓冲区是否满了

void OTA_Init(void)
{
    HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    printf("OTA 接收服务已启动，等待固件...\r\n");
}

// 【修改点1】中断里只做最简单的保存，绝不写 Flash！
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        last_rx_tick = HAL_GetTick(); 
        
        // 如果缓冲区还没满，就存进去
        if (ota_len < 1024) 
        {
            ota_buffer[ota_len++] = rx_byte;
            if (ota_len >= 1024) {
                buffer_full = true; // 满了，通知主循环去处理
            }
        }
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

// 【修改点2】把耗时操作放到主循环
void OTA_Task(void)
{
    // 1. 如果缓冲区满了，在这里慢慢写外挂 Flash
    if (buffer_full)
    {
        if (!ota_started) {
            printf("准备接收固件，擦除 W25Q64...\r\n");
            for(int i=0; i<30; i++) {
                SPI_FLASH_SectorErase(FIRMWARE_SAVE_ADDR + i*4096);
            }
            write_flash_addr = FIRMWARE_SAVE_ADDR;
            ota_started = true;
            printf("擦除完成，正在写入...\r\n");
        }
        
        SPI_FLASH_WriteBuffer(write_flash_addr, ota_buffer, 1024);
        write_flash_addr += 1024;
        ota_len = 0; // 清空缓冲区
        buffer_full = false; // 允许中断继续存
    }

    // 2. 如果开始接收了，且超过 2000ms 没收到新数据，说明发完了
    if (ota_started && (HAL_GetTick() - last_rx_tick > 2000) && !buffer_full) 
    {
        ota_started = false; 
        
        // 把最后不足 1024 的数据写进去
        if (ota_len > 0)
        {
            SPI_FLASH_WriteBuffer(write_flash_addr, ota_buffer, ota_len);
            ota_len = 0;
        }
        
        printf("固件接收完毕，写入标志位并重启！\r\n");
        uint8_t flag_arr[4] = {0xAA, 0xBB, 0x11, 0x22}; 
        Internal_FLASH_Erase(OTA_FLAG_ADDR, 4);
        Internal_FLASH_Write(OTA_FLAG_ADDR, flag_arr, 4);
        
        HAL_Delay(100); 
        NVIC_SystemReset(); 
    }
}

// 搬运和跳转函数保持原样
void JumpToApp(uint32_t app_addr)
{
    typedef void (*pFunction)(void);
    pFunction Jump_To_Application;
    uint32_t JumpAddress;

    if (((*(__IO uint32_t*)app_addr) & 0x2FFE0000 ) == 0x20000000) {
        JumpAddress = *(__IO uint32_t*)(app_addr + 4);
        Jump_To_Application = (pFunction) JumpAddress;
        __disable_irq(); 
        __set_MSP(*(__IO uint32_t*)app_addr); 
        Jump_To_Application(); 
    } else {
        printf("APP 区为空或非法，无法跳转！\r\n");
    }
}

/**
  * @brief  从 W25Q64 搬运固件到内部 Flash
  */
void UpdateApp(void)
{
    printf("开始搬运固件...\r\n");
    
    // 1. 擦除内部 Flash 的 APP 区 (假设 APP 最大 40KB，我们多擦除一点，比如从 0x08005000 开始擦除 40 页)
    // 注意：千万别把 Bootloader 自己(0x08000000-0x08005000)擦掉了！
    if (!Internal_FLASH_Erase(APP_START_ADDR, 40 * 1024)) {
        printf("内部 Flash 擦除失败！\r\n");
        return;
    }

    uint8_t buffer[1024]; // 每次搬运 1KB
    uint32_t read_addr = FIRMWARE_SAVE_ADDR;   // W25Q64 里的起始地址
    uint32_t write_addr = APP_START_ADDR;      // 内部 Flash 里的起始地址
    
    // 2. 循环搬运 (假设我们的固件大小是 30KB，通过宏定义或标志位获取实际大小，这里先写死循环搬 30 次)
    // 真实项目中，固件包头里会包含固件长度，这里为了演示，假设搬运 30KB
    for (int i = 0; i < 30; i++) 
    {
        // 从 W25Q64 读 1KB 数据
        SPI_FLASH_ReadBuffer(read_addr, buffer, 1024);
        
        // 写入内部 Flash
        Internal_FLASH_Write(write_addr, buffer, 1024);
        
        // 地址偏移
        read_addr += 1024;
        write_addr += 1024;
    }
    
    printf("搬运完成，清除标志位并跳转！\r\n");
    
    // 3. 清除标志位（非常重要，不然以后每次上电都会卡在搬运）
    uint8_t clear_flag[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    Internal_FLASH_Erase(OTA_FLAG_ADDR, 4);
    Internal_FLASH_Write(OTA_FLAG_ADDR, clear_flag, 4); // 或者不写，擦除后默认就是 0xFF

    HAL_Delay(100); // 留给串口打印时间
    JumpToApp(APP_START_ADDR); // 搬运完毕，直接跳转
}