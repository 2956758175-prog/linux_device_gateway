#ifndef __PROTOCOL_PARSER_H__
#define __PROTOCOL_PARSER_H__

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

// 接收缓冲区大小
#define PARSER_BUFFER_SIZE 4096

// 协议解析器
typedef struct {
    unsigned char buffer[PARSER_BUFFER_SIZE];
    size_t len;
} ProtocolParser;

// 完整协议帧
typedef struct {
    ProtocolHeader header;
    unsigned char payload[MAX_PAYLOAD_LEN];
} ProtocolFrame;

// 初始化解析器
void protocol_parser_init(ProtocolParser *parser);

// 向解析器追加接收到的数据
int protocol_parser_append(ProtocolParser *parser,
                           const unsigned char *data,
                           size_t len);

// 尝试提取一条完整协议帧
int protocol_parser_next(ProtocolParser *parser,
                         ProtocolFrame *frame);

#endif
