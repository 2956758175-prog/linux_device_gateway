
#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>
#include <stddef.h>

#define FRAME_HEADER 0xAA55
#define PROTOCOL_VERSION 0x01

#define CMD_REGISTER  0x01
#define CMD_HEARTBEAT 0x02
#define CMD_DATA      0x03
#define CMD_QUERY     0x04
#define CMD_CONTROL   0x05

#define CMD_RESPONSE 0x80

/* Server response status */
#define STATUS_SUCCESS          0x00
#define STATUS_ALREADY_ONLINE   0x01
#define STATUS_NOT_REGISTERED   0x02
#define STATUS_FAILED           0x03

#define MAX_PAYLOAD_LEN 1024

// 固定协议头，共 12 字节
typedef struct {
    uint16_t header;       // 帧头：2 字节
    uint8_t version;       // 版本：1 字节
    uint8_t command;       // 命令字：1 字节
    uint32_t device_id;    // 设备 ID：4 字节
    uint32_t data_len;     // 数据长度：4 字节
} ProtocolHeader;

#define PROTOCOL_HEADER_LEN 12

// 协议头编码
int encode_header(unsigned char *buf,
                  const ProtocolHeader *header);

// 协议头解码
int decode_header(const unsigned char *buf,ProtocolHeader *header);

#define CRC16_LEN 2

uint16_t crc16_ccitt(const unsigned char *data, size_t len);

#endif

