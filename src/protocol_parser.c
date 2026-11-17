#include <string.h>
#include "protocol_parser.h"

/*
 * 初始化协议解析器
 */
void protocol_parser_init(ProtocolParser *parser)
{
    if (parser == NULL) {
        return;
    }

    memset(parser, 0, sizeof(ProtocolParser));
}

/*
 * 向解析缓冲区追加数据
 */
int protocol_parser_append(ProtocolParser *parser,
                           const unsigned char *data,
                           size_t len)
{
    if (parser == NULL || (data == NULL && len > 0)) {
        return -1;
    }

    if (len > PARSER_BUFFER_SIZE - parser->len) {
        return -1;
    }

    if (len > 0) {
        memcpy(parser->buffer + parser->len, data, len);
        parser->len += len;
    }

    return 0;
}

/*
 * 从缓冲区中解析一帧完整协议数据
 *
 * 返回值：
 *   1：成功解析一帧
 *   0：数据不足，等待后续数据
 *  -1：参数错误或解析器状态异常
 */
int protocol_parser_next(ProtocolParser *parser,
                         ProtocolFrame *frame)
{
    ProtocolHeader header;
    size_t frame_len;

    if (parser == NULL || frame == NULL) {
        return -1;
    }

    if (parser->len > PARSER_BUFFER_SIZE) {
        return -1;
    }

    /*
     * 搜索协议帧头
     */
    while (parser->len >= 2) {

        uint16_t magic =
            ((uint16_t)parser->buffer[0] << 8) |
            parser->buffer[1];

        /*
         * 当前两个字节不是帧头，丢弃一个字节
         */
        if (magic != FRAME_HEADER) {

            memmove(parser->buffer,
                    parser->buffer + 1,
                    parser->len - 1);

            parser->len--;

            continue;
        }

        /*
         * 帧头存在，但协议头还没有接收完整
         */
        if (parser->len < PROTOCOL_HEADER_LEN) {
            return 0;
        }

        /*
         * 解码协议头
         */
        if (decode_header(parser->buffer, &header) < 0) {
            return -1;
        }

        /*
         * 检查协议版本和 Payload 长度
         */
        if (header.version != PROTOCOL_VERSION ||
            header.data_len > MAX_PAYLOAD_LEN) {

            memmove(parser->buffer,
                    parser->buffer + 1,
                    parser->len - 1);

            parser->len--;

            continue;
        }

        /*
         * 计算完整帧长度
         * 协议头 + Payload + CRC16
         */
        frame_len = PROTOCOL_HEADER_LEN +
                    header.data_len +
                    CRC16_LEN;

        /*
         * 完整帧尚未接收完毕
         */
        if (parser->len < frame_len) {
            return 0;
        }

        /*
         * 提取接收到的 CRC16
         * CRC 高字节在前，低字节在后
         */
        uint16_t received_crc =
            ((uint16_t)parser->buffer[frame_len - 2] << 8) |
            parser->buffer[frame_len - 1];

        /*
         * 计算 CRC16
         * 校验范围：协议头 + Payload
         */
        uint16_t calculated_crc =
            crc16_ccitt(parser->buffer,
                        PROTOCOL_HEADER_LEN + header.data_len);

        /*
         * CRC 校验失败，丢弃一个字节并重新搜索帧头
         */
        if (received_crc != calculated_crc) {

            memmove(parser->buffer,
                    parser->buffer + 1,
                    parser->len - 1);

            parser->len--;

            continue;
        }

        /*
         * CRC 校验成功，提取协议帧
         */
        frame->header = header;

        memcpy(frame->payload,
               parser->buffer + PROTOCOL_HEADER_LEN,
               header.data_len);

        /*
         * 移除已经解析的协议帧
         * 保留缓冲区中后续未解析的数据
         */
        memmove(parser->buffer,
                parser->buffer + frame_len,
                parser->len - frame_len);

        parser->len -= frame_len;

        return 1;
    }

    /*
     * 只剩一个字节时：
     * 如果是 0xAA，可能是帧头的第一个字节，保留；
     * 否则清空缓冲区。
     */
    if (parser->len == 1 &&
        parser->buffer[0] != 0xAA) {

        parser->len = 0;
    }

    return 0;
}