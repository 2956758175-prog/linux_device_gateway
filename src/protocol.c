
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

#include "protocol.h"

// 将协议头编码为 12 字节网络数据
int encode_header(unsigned char *buf,
                  const ProtocolHeader *header)
{
    uint16_t net_header;
    uint32_t net_device_id;
    uint32_t net_data_len;

    if (buf == NULL || header == NULL) {
        return -1;
    }

    // 转换为网络字节序
    net_header = htons(header->header);
    net_device_id = htonl(header->device_id);
    net_data_len = htonl(header->data_len);

    // 按照协议规定的偏移写入
    memcpy(buf, &net_header, 2);

    buf[2] = header->version;
    buf[3] = header->command;

    memcpy(buf + 4, &net_device_id, 4);
    memcpy(buf + 8, &net_data_len, 4);

    return PROTOCOL_HEADER_LEN;
}

// 将 12 字节网络数据解码为协议头
int decode_header(const unsigned char *buf,
                  ProtocolHeader *header)
{
    uint16_t net_header;
    uint32_t net_device_id;
    uint32_t net_data_len;

    if (buf == NULL || header == NULL) {
        return -1;
    }

    memcpy(&net_header, buf, 2);

    header->header = ntohs(net_header);
    header->version = buf[2];
    header->command = buf[3];

    memcpy(&net_device_id, buf + 4, 4);
    memcpy(&net_data_len, buf + 8, 4);

    header->device_id = ntohl(net_device_id);
    header->data_len = ntohl(net_data_len);

    return PROTOCOL_HEADER_LEN;
}

// CRC16-CCITT-FALSE
uint16_t crc16_ccitt(const unsigned char *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    if (data == NULL) {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {

        crc ^= (uint16_t)data[i] << 8;

        for (int j = 0; j < 8; j++) {

            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}
