
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"
#include "protocol_parser.h"

#define MAX_TEST_CHUNK 64

static unsigned int test_count = 0;

/* 生成测试 Payload */
static void generate_payload(unsigned char *payload, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        payload[i] = (unsigned char)(i * 17 + 31);
    }
}

/* 构造完整协议帧 */
static int generate_frame(
    unsigned char *buf,
    size_t *frame_len,
    uint32_t payload_len)
{
    ProtocolHeader header;

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = CMD_DATA;
    header.device_id = 5001;
    header.data_len = payload_len;

    if (payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }

    if (encode_header(buf, &header) < 0) {
        return -1;
    }

    if (payload_len > 0) {
        generate_payload(buf + PROTOCOL_HEADER_LEN, payload_len);
    }

    size_t crc_len = PROTOCOL_HEADER_LEN + payload_len;

    uint16_t crc = crc16_ccitt(buf, crc_len);

    buf[crc_len] = (unsigned char)(crc >> 8);
    buf[crc_len + 1] = (unsigned char)crc;

    *frame_len = crc_len + CRC16_LEN;

    return 0;
}

/* 检查解析出来的帧 */
static int check_frame(
    const ProtocolFrame *frame,
    uint32_t expected_payload_len)
{
    if (frame->header.device_id != 5001 ||
        frame->header.command != CMD_DATA ||
        frame->header.version != PROTOCOL_VERSION ||
        frame->header.data_len != expected_payload_len) {
        return -1;
    }

    for (uint32_t i = 0; i < expected_payload_len; i++) {
        unsigned char expected =
            (unsigned char)(i * 17 + 31);

        if (frame->payload[i] != expected) {
            return -1;
        }
    }

    return 0;
}

/*
 * 按指定 chunk_size 输入数据。
 * chunk_size=1 表示每次只追加一个字节。
 */
static int test_frame_split(
    uint32_t payload_len,
    size_t chunk_size)
{
    unsigned char buf[
        PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN
    ];

    size_t frame_len = 0;

    if (generate_frame(buf, &frame_len, payload_len) < 0) {
        return -1;
    }

    ProtocolParser parser;
    protocol_parser_init(&parser);

    size_t offset = 0;
    int parsed_count = 0;

    while (offset < frame_len) {

        size_t current_len = chunk_size;

        if (current_len > frame_len - offset) {
            current_len = frame_len - offset;
        }

        if (protocol_parser_append(
                &parser,
                buf + offset,
                current_len) < 0) {
            printf("Append failed: payload=%u chunk=%zu\n",
                   payload_len, chunk_size);
            return -1;
        }

        offset += current_len;

        ProtocolFrame frame;
        int ret;

        while ((ret = protocol_parser_next(
                    &parser, &frame)) == 1) {

            parsed_count++;

            if (parsed_count > 1) {
                printf("Unexpected extra frame!\n");
                return -1;
            }

            if (check_frame(&frame, payload_len) < 0) {
                printf("Frame content mismatch!\n");
                return -1;
            }
        }

        if (ret < 0) {
            printf("Parser error!\n");
            return -1;
        }
    }

    if (parsed_count != 1) {
        printf("Frame count mismatch: payload=%u count=%d\n",
               payload_len, parsed_count);
        return -1;
    }

    test_count++;

    return 0;
}

int main(void)
{
    const uint32_t payload_lengths[] = {
        0,
        1,
        MAX_PAYLOAD_LEN
    };

    const size_t chunk_sizes[] = {
        1,
        2,
        3,
        12,
        16,
        32,
        MAX_TEST_CHUNK
    };

    printf("========== Protocol Boundary Test ==========\n");

    for (size_t i = 0;
         i < sizeof(payload_lengths) / sizeof(payload_lengths[0]);
         i++) {

        for (size_t j = 0;
             j < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);
             j++) {

            uint32_t payload_len = payload_lengths[i];
            size_t chunk_size = chunk_sizes[j];

            printf("Testing payload=%u, chunk=%zu ... ",
                   payload_len, chunk_size);

            if (test_frame_split(payload_len, chunk_size) < 0) {
                printf("FAILED\n");
                return 1;
            }

            printf("PASSED\n");
        }
    }

    printf("\nTotal tests: %u\n", test_count);
    printf("Protocol boundary test PASSED.\n");

    return 0;
}
