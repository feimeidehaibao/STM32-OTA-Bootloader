/**
  ******************************************************************************
  * @file       flash.c
  * @author     embedfire (底层驱动原始框架)
  * @modifier   Yang Chenghao (杨承澔) 

  ******************************************************************************
  */
  
#include "flash.h"
#include "spi.h"
#include <stdio.h>
extern SPI_HandleTypeDef hspi1;  // HAL SPI 句柄



uint16_t SPI_FLASH_ReadDeviceID(void)
{
    uint8_t tx_buf[6] = {0};        // 发送缓存
    uint8_t rx_buf[6] = {0};        // 接收缓存
    uint16_t id;

    tx_buf[0] = W25Q64_MANUFACTURER_DEVICE_ID;                     // 读取制造商设备ID指令
    tx_buf[1] = (uint8_t)((W25Q64_MANUFACTDEVICEID_ADDRESS >> 16) & 0xFF);  // 地址高8位
    tx_buf[2] = (uint8_t)((W25Q64_MANUFACTDEVICEID_ADDRESS >> 8) & 0xFF);   // 地址中8位
    tx_buf[3] = (uint8_t)(W25Q64_MANUFACTDEVICEID_ADDRESS & 0xFF);          // 地址低8位
    tx_buf[4] = 0x00;  // 哑元字节1
    tx_buf[5] = 0x00;  // 哑元字节2

    FLASH_CS_LOW();   // 拉低片选，开始通信
    if (HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 6, HAL_MAX_DELAY) != HAL_OK)  // 发送命令并接收数据
    {
        FLASH_CS_HIGH();  // 失败拉高片选，结束通信
        printf("错误：发送接收制造商/设备ID失败！\r\n");
        return 0;
    }
    FLASH_CS_HIGH();  // 通信结束，拉高片选

    id = ((uint16_t)rx_buf[4] << 8) | rx_buf[5];  // 组合接收到的ID高低字节
    return id;
}


/**
  * @brief  读取W25Q64芯片JEDEC ID
  * @retval 24位JEDEC ID，读取失败返回0
  */
uint32_t SPI_FLASH_JEDEC_ID(void)
{
    uint8_t tx_buf[4] = {W25Q64_JEDEC_ID, 0x00, 0x00, 0x00};  // 发送缓存，包含读取JEDEC ID命令和3个哑元字节
    uint8_t rx_buf[4] = {0};                                  // 接收缓存

    FLASH_CS_LOW();  // 拉低片选，开始通信
    if (HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 4, HAL_MAX_DELAY) != HAL_OK)  // 发送命令并接收数据
    {
        FLASH_CS_HIGH();  // 失败拉高片选，结束通信
        printf("错误：发送接收JEDEC ID失败！\r\n");
        return 0;
    }
    FLASH_CS_HIGH();  // 通信结束，拉高片选

    uint32_t id = ((uint32_t)rx_buf[1] << 16) | ((uint32_t)rx_buf[2] << 8) | rx_buf[3];  // 组合JEDEC ID高、中、低字节
    return id;
}

/**
  * @brief  使能写操作，发送写使能命令
  */
void SPI_FLASH_WriteEnable(void)
{
    uint8_t cmd = W25Q64_WRITE_ENABLE;  // 写使能命令
    FLASH_CS_LOW();                     // 拉低片选，开始通信

    if (HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK)  // 发送命令
    {
        printf("错误：发送写使能命令失败！\r\n");
    }

    FLASH_CS_HIGH();                    // 拉高片选，结束通信
}

/**
  * @brief  等待写操作完成，轮询状态寄存器WIP位
  */
void SPI_FLASH_WaitForWriteEnd(void)
{
    uint8_t cmd = W25Q64_READ_STATUS_REGISTER_1;  // 读取状态寄存器命令
    uint8_t status = 0;

    FLASH_CS_LOW();                                // 拉低片选，开始通信

    if (HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK)  // 发送读取状态寄存器命令
    {
        printf("错误：发送读取状态寄存器命令失败！\r\n");
    }

    do {
        if (HAL_SPI_Receive(&hspi1, &status, 1, HAL_MAX_DELAY) != HAL_OK)  // 接收状态寄存器值
        {
            printf("错误：接收状态寄存器失败！\r\n");
            break;
        }
    } while ((status & WIP_FLAG) == SET);                    // 等待写操作完成（WIP位清零）

    FLASH_CS_HIGH();                               // 拉高片选，结束通信
}

/**
  * @brief  进入掉电模式，降低功耗
  */
void SPI_FLASH_PowerDown(void)
{
    uint8_t cmd = W25Q64_POWER_DOWN;  // 掉电命令
    FLASH_CS_LOW();                   // 拉低片选，开始通信

    if (HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK)  // 发送命令
    {
        printf("SPI 发送掉电命令失败\r\n");
    }

    FLASH_CS_HIGH();                  // 拉高片选，结束通信
}

/**
  * @brief  唤醒FLASH，退出掉电模式
  */
void SPI_FLASH_WakeUp(void)
{
    uint8_t cmd = W25Q64_RELEASE_POWER_DOWN_HPM_DEVICE_ID;  // 唤醒命令
    FLASH_CS_LOW();                                         // 拉低片选，开始通信

    if (HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK)  // 发送命令
    {
        printf("SPI 发送唤醒命令失败\r\n");
    }

    FLASH_CS_HIGH();                                        // 拉高片选，结束通信
}

/**
  * @brief  批量擦除整个芯片
  */
void SPI_FLASH_BulkErase(void)
{
    uint8_t cmd = W25Q64_CHIP_ERASE;  // 芯片擦除命令
    SPI_FLASH_WriteEnable();           // 写使能

    FLASH_CS_LOW();                   // 拉低片选，开始通信

    if (HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK)  // 发送擦除命令
    {
        printf("SPI 发送芯片擦除命令失败\r\n");
    }

    FLASH_CS_HIGH();                  // 拉高片选，结束通信

    SPI_FLASH_WaitForWriteEnd();     // 等待擦除完成
}

/**
  * @brief  擦除指定扇区（4KB）
  * @param  sector_addr 扇区地址（字节地址）
  */
void SPI_FLASH_SectorErase(uint32_t sector_addr)
{
    uint8_t cmd_addr[4];
    cmd_addr[0] = W25Q64_SECTOR_ERASE_4KB;       // 扇区擦除命令
    cmd_addr[1] = (sector_addr >> 16) & 0xFF;    // 地址高8位
    cmd_addr[2] = (sector_addr >> 8) & 0xFF;     // 地址中8位
    cmd_addr[3] = sector_addr & 0xFF;            // 地址低8位

    SPI_FLASH_WriteEnable();                      // 写使能

    FLASH_CS_LOW();                               // 拉低片选，开始通信
    if (HAL_SPI_Transmit(&hspi1, cmd_addr, 4, HAL_MAX_DELAY) != HAL_OK)  // 发送命令和地址
    {
        printf("SPI 发送扇区擦除命令失败\r\n");
    }
    FLASH_CS_HIGH();                              // 拉高片选，结束通信

    SPI_FLASH_WaitForWriteEnd();                  // 等待擦除完成
}

/**
  * @brief  向指定页写入数据，大小不能超过一页
  * @param  write_addr 写入起始地址（必须在同一页内）
  * @param  buffer 待写入的数据指针
  * @param  size 写入数据长度（字节数），不能跨页
  */
void SPI_FLASH_WritePage(uint32_t write_addr, uint8_t* buffer, uint16_t size)
{
    // 检查是否跨页，跨页不允许写入
    if(size + (write_addr % W25Q64_SPI_FLASH_PAGESIZE) > W25Q64_SPI_FLASH_PAGESIZE)
        return;

    uint8_t cmd_addr[4];
    cmd_addr[0] = W25Q64_PAGE_PROGRAM;           // 页编程命令
    cmd_addr[1] = (write_addr >> 16) & 0xFF;     // 地址高8位
    cmd_addr[2] = (write_addr >> 8) & 0xFF;      // 地址中8位
    cmd_addr[3] = write_addr & 0xFF;              // 地址低8位

    SPI_FLASH_WriteEnable();                      // 写使能

    FLASH_CS_LOW();                               // 片选拉低，开始通信
    if (HAL_SPI_Transmit(&hspi1, cmd_addr, 4, HAL_MAX_DELAY) != HAL_OK)   // 发送写命令和地址
    {
        printf("SPI 发送页写入命令失败\r\n");
    }
    if (HAL_SPI_Transmit(&hspi1, buffer, size, HAL_MAX_DELAY) != HAL_OK)  // 发送写入数据
    {
        printf("SPI 发送写入数据失败\r\n");
    }
    FLASH_CS_HIGH();                              // 片选拉高，结束通信

    SPI_FLASH_WaitForWriteEnd();                  // 等待写入完成
}

/**
  * @brief  向FLASH写入指定长度的数据，自动处理跨页写入
  * @param  write_addr 写入起始地址
  * @param  buffer 待写入数据指针
  * @param  size 写入数据长度（字节数）
  */
void SPI_FLASH_WriteBuffer(uint32_t write_addr, uint8_t* buffer, uint32_t size)
{
    uint32_t write_num = size;                     // 剩余写入字节数
    uint32_t current_addr = write_addr;            // 当前写入地址
    uint32_t offset = write_addr % W25Q64_SPI_FLASH_PAGESIZE;  // 当前地址在页内的偏移量

    // 如果起始地址不是页起始地址，先写入页剩余空间的数据
    if(offset != 0) {
        uint32_t page_remain = W25Q64_SPI_FLASH_PAGESIZE - offset;  // 当前页剩余空间
        if(write_num > page_remain) {
            SPI_FLASH_WritePage(current_addr, buffer, page_remain); // 写满当前页剩余空间
            current_addr += page_remain;       // 地址后移
            buffer += page_remain;             // 数据指针后移
            write_num -= page_remain;          // 剩余数据减少
        } else {
            SPI_FLASH_WritePage(current_addr, buffer, write_num);   // 数据不足一页，直接写入
            return;
        }
    }

    // 按整页写入剩余数据
    while(write_num > 0) {
        uint16_t write_size = (write_num > W25Q64_SPI_FLASH_PAGESIZE) ? W25Q64_SPI_FLASH_PAGESIZE : write_num;  // 本次写入大小
        SPI_FLASH_WritePage(current_addr, buffer, write_size);  // 写入一页数据
        current_addr += write_size;     // 地址后移
        buffer += write_size;           // 数据指针后移
        write_num -= write_size;        // 剩余数据减少
    }
}

/**
  * @brief  从指定地址读取数据到缓冲区
  * @param  read_addr 读取起始地址
  * @param  buffer 存放读取数据的缓冲区指针
  * @param  size 要读取的数据长度（字节数）
  */
void SPI_FLASH_ReadBuffer(uint32_t read_addr, uint8_t* buffer, uint32_t size)
{
    uint8_t cmd_addr[4];
    cmd_addr[0] = W25Q64_READ_DATA;              // 读取数据命令
    cmd_addr[1] = (read_addr >> 16) & 0xFF;      // 地址高8位
    cmd_addr[2] = (read_addr >> 8) & 0xFF;       // 地址中8位
    cmd_addr[3] = read_addr & 0xFF;              // 地址低8位

    FLASH_CS_LOW();                              // 片选拉低，开始通信
    if (HAL_SPI_Transmit(&hspi1, cmd_addr, 4, HAL_MAX_DELAY) != HAL_OK)  // 发送读取命令和地址
    {
        printf("SPI 发送读取命令失败\r\n");
    }
    if (HAL_SPI_Receive(&hspi1, buffer, size, HAL_MAX_DELAY) != HAL_OK)  // 接收读取的数据
    {
        printf("SPI 接收读取数据失败\r\n");
    }
    FLASH_CS_HIGH();                             // 片选拉高，结束通信
}

/**************** 内部 STM32 Flash 读写实现 ****************/

/**
  * @brief  从内部 Flash 读取数据
  */
bool Internal_FLASH_Read(uint32_t readAddr, uint8_t *pBuffer, uint32_t numToRead)
{	
	if ((readAddr + numToRead) > STM32_FLASH_END) {
		return false;
	}
	
	uint32_t addr = readAddr;
	for (uint32_t i = 0; i < numToRead; i++) 
	{
		*pBuffer = *(uint8_t *)addr;
		addr = addr + 1;
		pBuffer++;
	}
	return true;
}

/**
  * @brief  向内部 Flash 写入数据 (F1 必须按半字/16位写入)
  */
bool Internal_FLASH_Write(uint32_t writeAddr, uint8_t *pBuffer, uint32_t numToWrite)
{	
	if ((writeAddr + numToWrite) > STM32_FLASH_END) {
		return false;
	}
	if (writeAddr % 2 == 1) {  // 半字(2字节)写入，地址必须对齐
		return false;
	}
    
	uint16_t temp;
	HAL_FLASH_Unlock();   
	for (uint32_t i = 0; i < numToWrite / 2; i++) 
	{
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, writeAddr, *(uint16_t *)pBuffer);
		pBuffer += 2;
		writeAddr += 2;
	}
	if (numToWrite % 2) // 如果是奇数长度，补 0xFF
	{
		temp = *pBuffer | 0xff00;
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, writeAddr, temp);
	}
	HAL_FLASH_Lock();
	return true;
}

/**
  * @brief  擦除内部 Flash (自动处理跨页)
  */
bool Internal_FLASH_Erase(uint32_t eraseAddr, uint32_t numToErase)
{
	if (numToErase == 0 || (eraseAddr + numToErase) > STM32_FLASH_END) {
		return false;
	} 
	FLASH_EraseInitTypeDef flashEraseStruct;
	uint32_t pageError;
	uint32_t addrOffset = eraseAddr - STM32_FLASH_BASE;          
    uint16_t pageOffset = addrOffset % STM32_PAGE_SIZE;             

	HAL_FLASH_Unlock(); 

	if (numToErase > (STM32_PAGE_SIZE - pageOffset)) // 跨页
	{
		flashEraseStruct.TypeErase = FLASH_TYPEERASE_PAGES;     
		flashEraseStruct.NbPages = 1;                           
		flashEraseStruct.PageAddress = eraseAddr - pageOffset;  
		if (HAL_OK != HAL_FLASHEx_Erase(&flashEraseStruct, &pageError)) goto erase_err;

		eraseAddr += STM32_PAGE_SIZE - pageOffset;             
		numToErase -= (STM32_PAGE_SIZE - pageOffset);
		uint16_t pageNum = numToErase / STM32_PAGE_SIZE;
		
		if (numToErase % STM32_PAGE_SIZE == 0)
		{
			flashEraseStruct.NbPages = pageNum;                    
			flashEraseStruct.PageAddress = eraseAddr;              
			if (HAL_OK != HAL_FLASHEx_Erase(&flashEraseStruct, &pageError)) goto erase_err;		
		}
		else
		{
			flashEraseStruct.NbPages = pageNum + 1;                
			flashEraseStruct.PageAddress = eraseAddr;              
			if (HAL_OK != HAL_FLASHEx_Erase(&flashEraseStruct, &pageError)) goto erase_err;
		}
	}
	else  // 没有跨页
	{
		flashEraseStruct.TypeErase = FLASH_TYPEERASE_PAGES;     
		flashEraseStruct.NbPages = 1;                           
		flashEraseStruct.PageAddress = eraseAddr - pageOffset;  
		if (HAL_OK != HAL_FLASHEx_Erase(&flashEraseStruct, &pageError)) goto erase_err;
	}
    HAL_FLASH_Lock();
	return true;

erase_err:
    HAL_FLASH_Lock();
	return false;
}