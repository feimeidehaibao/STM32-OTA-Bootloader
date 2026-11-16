#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define HEAD_SIZE 12 //设置包头长度
#define MAX_SIZE  (1024 * 1024) //设置固件最大长度
static uint8_t g_data[MAX_SIZE]; // 全局区防爆栈

// 1.核心 CRC32 算法
static uint32_t crc32_calc(const uint8_t *p, uint32_t len) 
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// 小端写入工具,使四字节的数据可以写入单字节的数组中
static void put_u32(uint8_t *p, uint32_t v) 
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;//将四字节的数据分成四个单字节的数据，分别作为数组的四个元素
}

int main(int argc, char *argv[]) {
    // 1. 检查参数
    if (argc != 3) {
        printf("Usage: %s <input.bin> <output_ota.bin>\n", argv[0]);
        return -1;
    }

    // 2. 二进制读取
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { printf("Error: Cannot open input file.\n"); return -1; }
    uint32_t size = (uint32_t)fread(g_data, 1, MAX_SIZE, fp);
    fclose(fp);
    if (size == 0) { printf("Error: Empty file.\n"); return -1; }

    // 3. 计算 CRC
    uint32_t crc = crc32_calc(g_data, size);

    // 4. 组装 12 字节包头
    uint8_t head[HEAD_SIZE];
    memcpy(head, "OTA1", 4);
    put_u32(head + 4, size);
    put_u32(head + 8, crc);

    // 5. 二进制写入
    fp = fopen(argv[2], "wb");
    if (!fp) { printf("Error: Cannot create output file.\n"); return -1; }
    
    fwrite(head, 1, HEAD_SIZE, fp);
    fwrite(g_data, 1, size, fp);
    fclose(fp);

    printf("Success! %s (%u bytes) generated. CRC32: 0x%08X\n", argv[2], size + HEAD_SIZE, crc);
    return 0;
}