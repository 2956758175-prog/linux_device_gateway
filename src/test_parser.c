#include <stdio.h>
#include <string.h>

#include "protocol_parser.h"

// 构造一条完整协议帧
static size_t build_frame(unsigned char *buf,const char *payload,uint32_t device_id)
{
    ProtocolHeader header;

    size_t payload_len = strlen(payload);

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = CMD_DATA;
    header.device_id = device_id;
    header.data_len = (uint32_t)payload_len;

    // 编码协议头
    if (encode_header(buf, &header) < 0) {
        return 0;
    }

    // 追加 Payload
    memcpy(buf + PROTOCOL_HEADER_LEN,
           payload,
           payload_len);

    // 计算 CRC
    size_t crc_offset = PROTOCOL_HEADER_LEN + payload_len;

    uint16_t crc = crc16_ccitt(buf, crc_offset);

    // 写入 CRC
    buf[crc_offset] = (unsigned char)(crc >> 8);
    buf[crc_offset + 1] = (unsigned char)(crc & 0xFF);

    return crc_offset + CRC16_LEN;
}

// 测试一：拆包
static void test_split(void)
{
    printf("\n========== Test 1: TCP Split ==========\n");

    ProtocolParser parser;
    ProtocolFrame frame;

    unsigned char buf[PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN];

    size_t len = build_frame(buf, "temperature=25.6", 1001);

    protocol_parser_init(&parser);

    // 第一次只追加前 5 个字节
    protocol_parser_append(&parser, buf, 5);

    int ret = protocol_parser_next(&parser, &frame);

    printf("After first append: ret = %d\n", ret);

    // 第二次追加剩余数据
    protocol_parser_append(&parser, buf + 5, len - 5);

    ret = protocol_parser_next(&parser, &frame);

    printf("After second append: ret = %d\n", ret);

    if (ret == 1) {
        printf("Device ID: %u\n", frame.header.device_id);
        printf("Payload: %.*s\n",
               (int)frame.header.data_len,
               frame.payload);
    }

    printf("======================================\n");
}

// 测试二：粘包
static void test_sticky(void)
{
    printf("\n========== Test 2: TCP Sticky ==========\n");

    ProtocolParser parser;
    ProtocolFrame frame;

    unsigned char buf[2 * (PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN)];

    size_t len1 = build_frame(buf, "temperature=25.6", 1001);

    size_t len2 = build_frame(buf + len1, "humidity=60", 1002);

    protocol_parser_init(&parser);

    // 一次性追加两条完整协议帧
    protocol_parser_append(&parser, buf, len1 + len2);

    // 提取第一条协议帧
    int ret = protocol_parser_next(&parser, &frame);

    if (ret == 1) {
        printf("Frame 1: Device ID = %u, Payload = %.*s\n",
               frame.header.device_id,
               (int)frame.header.data_len,
               frame.payload);
    }

    // 提取第二条协议帧
    ret = protocol_parser_next(&parser, &frame);

    if (ret == 1) {
        printf("Frame 2: Device ID = %u, Payload = %.*s\n",
               frame.header.device_id,
               (int)frame.header.data_len,
               frame.payload);
    }

    printf("Remaining buffer length: %zu\n", parser.len);

    printf("========================================\n");
}

// 测试三：不完整协议帧
static void test_incomplete(void)
{
    printf("\n========== Test 3: Incomplete Frame ==========\n");

    ProtocolParser parser;
    ProtocolFrame frame;

    unsigned char buf[PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN];

    size_t len = build_frame(buf, "battery=85", 1003);

    protocol_parser_init(&parser);

    // 只追加协议头和部分 Payload
    size_t first_len = PROTOCOL_HEADER_LEN + 3;

    protocol_parser_append(&parser, buf, first_len);

    int ret = protocol_parser_next(&parser, &frame);

    printf("Before completion: ret = %d\n", ret);

    // 追加剩余 Payload
    protocol_parser_append(&parser,
                          buf + first_len,
                          len - first_len);

    ret = protocol_parser_next(&parser, &frame);

    printf("After completion: ret = %d\n", ret);

    if (ret == 1) {
        printf("Device ID: %u\n", frame.header.device_id);
        printf("Payload: %.*s\n",
               (int)frame.header.data_len,
               frame.payload);
    }

    printf("=============================================\n");
}

// 测试四：非法数据与帧头重新同步
static void test_resync(void)
{
    printf("\n========== Test 4: Frame Resync ==========\n");

    ProtocolParser parser;
    ProtocolFrame frame;

    unsigned char buf[PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN];

    unsigned char test_data[256];

    size_t len = build_frame(buf, "temperature=26.8", 1004);

    protocol_parser_init(&parser);

    // 在合法协议帧前面添加三个无效字节
    test_data[0] = 0x11;
    test_data[1] = 0x22;
    test_data[2] = 0x33;

    memcpy(test_data + 3, buf, len);

    // 一次性追加无效数据和合法协议帧
    protocol_parser_append(&parser, test_data, len + 3);

    int ret = protocol_parser_next(&parser, &frame);

    printf("Parser return: %d\n", ret);

    if (ret == 1) {
        printf("Device ID: %u\n", frame.header.device_id);

        printf("Payload: %.*s\n",
               (int)frame.header.data_len,
               frame.payload);
    }

    printf("Remaining buffer length: %zu\n", parser.len);

    printf("==========================================\n");
}

// 测试五：CRC 校验失败
static void test_crc_error(void)
{
    printf("\n========== Test 5: CRC Error ==========\n");

    ProtocolParser parser;
    ProtocolFrame frame;

    unsigned char buf[PROTOCOL_HEADER_LEN +
                      MAX_PAYLOAD_LEN +
                      CRC16_LEN];

    size_t len = build_frame(buf, "temperature=30", 1005);

    protocol_parser_init(&parser);

    // 故意修改 Payload 中的一个字节
    buf[PROTOCOL_HEADER_LEN] ^= 0x01;

    // 追加被篡改的协议帧
    protocol_parser_append(&parser, buf, len);

    int ret = protocol_parser_next(&parser, &frame);

    printf("Parser return: %d\n", ret);

    printf("Remaining buffer length: %zu\n", parser.len);

    printf("=======================================\n");
}

int main(void)
{
    test_split();

    test_sticky();

    test_incomplete();

    test_resync();

    test_crc_error();

    return 0;
}
