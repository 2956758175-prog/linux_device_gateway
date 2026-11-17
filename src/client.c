
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <time.h>

#include "protocol.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define DEVICE_ID 1001

#define INPUT_SIZE 1024

/*
 * 确保所有数据都发送出去
 */
int send_all(int sockfd, const unsigned char *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {

        ssize_t n = send(sockfd,
                         buf + total,
                         len - total,
                         MSG_NOSIGNAL);

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("send");
            return -1;
        }

        if (n == 0) {
            printf("send returned 0.\n");
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}


/*
 * 确保接收指定长度的数据
 */
int recv_all(int sockfd, unsigned char *buffer, size_t len)
{
    size_t total = 0;

    while (total < len) {

        ssize_t n = recv(sockfd,
                         buffer + total,
                         len - total,
                         0);

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("recv");
            return -1;
        }

        if (n == 0) {
            printf("Server disconnected.\n");
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}


/*
 * 解码 8 字节大端序 uint64_t
 */
static uint64_t decode_u64(const unsigned char *buf)
{
    uint64_t value = 0;

    for (int i = 0; i < 8; i++) {
        value = (value << 8) | buf[i];
    }

    return value;
}


/*
 * 打印普通业务响应状态
 */
void print_status(uint8_t status)
{
    switch (status) {

    case STATUS_SUCCESS:
        printf("Result: Operation successful.\n");
        break;

    case STATUS_ALREADY_ONLINE:
        printf("Result: Device is already registered and online.\n");
        break;

    case STATUS_NOT_REGISTERED:
        printf("Result: Device is not registered or offline.\n");
        break;

    case STATUS_FAILED:
        printf("Result: Operation failed.\n");
        break;

    default:
        printf("Result: Unknown status code.\n");
        break;
    }
}


/*
 * 解析并打印设备查询结果
 *
 * Payload 格式：
 *
 * status      1 字节
 * count       1 字节
 *
 * 每个设备：
 * device_id   4 字节
 * online      1 字节
 * last_seen   8 字节
 */
int parse_query_response(const unsigned char *payload,
                         uint32_t payload_len)
{
    if (payload_len < 2) {
        printf("Invalid query response: payload too short.\n");
        return -1;
    }

    uint8_t status = payload[0];
    uint8_t count = payload[1];

    size_t expected_len = 2 + (size_t)count * 13;

    if (payload_len != expected_len) {
        printf("Invalid query response length!\n");
        return -1;
    }

    printf("\n========== Device Query Result ==========\n");

    printf("Status: 0x%02X\n", status);
    printf("Device Count: %u\n", count);

    if (status == STATUS_NOT_REGISTERED) {
        printf("Target device does not exist.\n");
        printf("=========================================\n");
        return 0;
    }

    if (status != STATUS_SUCCESS) {
        printf("Query failed.\n");
        printf("=========================================\n");
        return 0;
    }

    if (count == 0) {
        printf("No devices registered.\n");
    }

    size_t offset = 2;

    for (uint8_t i = 0; i < count; i++) {

        uint32_t network_id;

        memcpy(&network_id, payload + offset, 4);

        uint32_t device_id = ntohl(network_id);

        offset += 4;

        uint8_t online = payload[offset++];

        uint64_t last_seen = decode_u64(payload + offset);

        offset += 8;

        printf("\n---------- Device %u ----------\n", i + 1);

        printf("Device ID: %u\n", device_id);

        printf("Online: %s\n",
               online ? "YES" : "NO");

        printf("Last Seen: %" PRIu64 "\n", last_seen);

        if (last_seen != 0) {

            time_t timestamp = (time_t)last_seen;

            char *time_text = ctime(&timestamp);

            if (time_text != NULL) {
                printf("Last Seen Time: %s", time_text);
            }
        }

        printf("------------------------------\n");
    }

    printf("=========================================\n");

    return 0;
}


/*
 * 接收并解析服务端响应帧
 *
 * 先接收固定 12 字节协议头，
 * 再根据 data_len 接收 Payload 和 CRC。
 */
int receive_response(int sockfd,
                     uint32_t expected_device_id,
                     uint8_t request_command)
{
    unsigned char frame[
        PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN
    ];

    ProtocolHeader header;

    /*
     * 第一步：接收固定长度协议头
     */
    if (recv_all(sockfd,
                 frame,
                 PROTOCOL_HEADER_LEN) < 0) {
        return -1;
    }

    if (decode_header(frame, &header) < 0) {
        printf("Invalid response header.\n");
        return -1;
    }

    /*
     * 第二步：检查协议头
     */
    if (header.header != FRAME_HEADER ||
        header.version != PROTOCOL_VERSION ||
        header.command != CMD_RESPONSE ||
        header.device_id != expected_device_id ||
        header.data_len > MAX_PAYLOAD_LEN) {

        printf("Invalid response frame header.\n");
        return -1;
    }

    /*
     * 第三步：根据 data_len 接收 Payload 和 CRC
     */
    size_t remaining_len =
        (size_t)header.data_len + CRC16_LEN;

    if (recv_all(sockfd,
                 frame + PROTOCOL_HEADER_LEN,
                 remaining_len) < 0) {
        return -1;
    }

    /*
     * 第四步：CRC 校验
     */
    size_t crc_offset =
        PROTOCOL_HEADER_LEN + header.data_len;

    uint16_t received_crc =
        ((uint16_t)frame[crc_offset] << 8) |
        frame[crc_offset + 1];

    uint16_t calculated_crc =
        crc16_ccitt(frame, crc_offset);

    if (received_crc != calculated_crc) {
        printf("Response CRC check failed!\n");
        return -1;
    }

    /*
     * 第五步：检查响应 Payload 长度
     */
    if (request_command == CMD_QUERY) {

        if (header.data_len < 2) {
            printf("Invalid query response payload.\n");
            return -1;
        }

    } else {

        if (header.data_len != 1) {
            printf("Invalid normal response payload.\n");
            return -1;
        }
    }

    uint8_t status = frame[PROTOCOL_HEADER_LEN];

    printf("\n========== Server Response ==========\n");
    printf("Device ID: %u\n", header.device_id);
    printf("Command: 0x%02X\n", header.command);
    printf("Payload Length: %u\n", header.data_len);
    printf("Status: 0x%02X\n", status);

    /*
     * 查询命令：解析可变长度设备信息
     */
    if (request_command == CMD_QUERY) {

        int ret = parse_query_response(
            frame + PROTOCOL_HEADER_LEN,
            header.data_len
        );

        printf("=====================================\n");

        return ret;
    }

    /*
     * 普通命令：只解析一个状态字节
     */
    print_status(status);

    printf("=====================================\n");

    return 0;
}


/*
 * 组装并发送协议帧
 *
 * request_device_id：
 * 普通设备命令传 DEVICE_ID
 * 查询命令传目标设备 ID
 */
int send_protocol_frame(int sockfd,
                        uint8_t command,
                        uint32_t request_device_id,
                        const unsigned char *payload,
                        uint32_t payload_len)
{
    unsigned char frame[
        PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN
    ];

    ProtocolHeader header;

    if (payload_len > MAX_PAYLOAD_LEN) {
        printf("Payload too large!\n");
        return -1;
    }

    if (payload_len > 0 && payload == NULL) {
        printf("Invalid payload!\n");
        return -1;
    }

    /*
     * 填充协议头
     */
    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = command;
    header.device_id = request_device_id;
    header.data_len = payload_len;

    if (encode_header(frame, &header) < 0) {
        printf("Encode header failed!\n");
        return -1;
    }

    /*
     * 拷贝 Payload
     */
    if (payload_len > 0) {

        memcpy(frame + PROTOCOL_HEADER_LEN,
               payload,
               payload_len);
    }

    /*
     * 计算 CRC16
     */
    size_t crc_offset =
        PROTOCOL_HEADER_LEN + payload_len;

    uint16_t crc = crc16_ccitt(frame, crc_offset);

    frame[crc_offset] = (unsigned char)(crc >> 8);

    frame[crc_offset + 1] =
        (unsigned char)(crc & 0xFF);

    /*
     * 计算完整帧长度
     */
    size_t frame_len = crc_offset + CRC16_LEN;

    /*
     * 发送完整协议帧
     */
    if (send_all(sockfd, frame, frame_len) < 0) {
        return -1;
    }

    printf("\nFrame sent successfully.\n");
    printf("Device ID: %u\n", request_device_id);
    printf("Command: 0x%02X\n", command);
    printf("Payload Length: %u\n", payload_len);

    /*
     * 等待服务端响应
     */
    if (receive_response(sockfd,
                         request_device_id,
                         command) < 0) {
        return -1;
    }

    return 0;
}


/*
 * 解析用户输入的 uint32_t 设备 ID
 */
int parse_device_id(const char *input, uint32_t *device_id)
{
    if (input == NULL || device_id == NULL) {
        return -1;
    }

    errno = 0;

    char *end = NULL;

    unsigned long value = strtoul(input, &end, 10);

    if (input == end || errno == ERANGE ||
        value > UINT32_MAX) {
        return -1;
    }

    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }

    if (*end != '\0') {
        return -1;
    }

    *device_id = (uint32_t)value;

    return 0;
}


int main(void)
{
    int sockfd;

    int exit_code = EXIT_SUCCESS;

    struct sockaddr_in server_addr;

    char input[INPUT_SIZE];

    /*
     * 1. 创建 Socket
     */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * 2. 初始化服务器地址
     */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_addr.sin_addr) <= 0) {

        perror("inet_pton");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /*
     * 3. 连接服务器
     */
    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {

        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to server.\n");
    printf("Device ID: %u\n", DEVICE_ID);

    /*
     * 4. 命令交互循环
     */
    while (1) {

        int choice;

        printf("\n========== Device Client ==========\n");
        printf("1. Register Device\n");
        printf("2. Send Heartbeat\n");
        printf("3. Send Data\n");
        printf("4. Exit\n");
        printf("5. Query Device\n");
        printf("Please select: ");

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }

        if (sscanf(input, "%d", &choice) != 1) {
            printf("Invalid input!\n");
            continue;
        }

        switch (choice) {

        case 1:

            printf("\nSending device registration...\n");

            if (send_protocol_frame(sockfd,
                                     CMD_REGISTER,
                                     DEVICE_ID,
                                     NULL,
                                     0) < 0) {

                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            break;

        case 2:

            printf("\nSending heartbeat...\n");

            if (send_protocol_frame(sockfd,
                                    CMD_HEARTBEAT,
                                    DEVICE_ID,
                                    NULL,
                                    0) < 0) {

                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            break;

        case 3: {

            printf("Please input data: ");

            if (fgets(input, sizeof(input), stdin) == NULL) {
                goto cleanup;
            }

            input[strcspn(input, "\n")] = '\0';

            if (send_protocol_frame(sockfd,
                                    CMD_DATA,
                                    DEVICE_ID,
                                    (const unsigned char *)input,
                                    (uint32_t)strlen(input)) < 0) {

                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            break;
        }

        case 4:

            printf("Client exiting...\n");
            goto cleanup;

        case 5: {

            uint32_t target_id;

            printf("Please input target device ID (0 = all devices): ");

            if (fgets(input, sizeof(input), stdin) == NULL) {
                goto cleanup;
            }

            if (parse_device_id(input, &target_id) < 0) {
                printf("Invalid device ID!\n");
                continue;
            }

            printf("\nSending device query...\n");

            if (send_protocol_frame(sockfd,
                                    CMD_QUERY,
                                    target_id,
                                    NULL,
                                    0) < 0) {

                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            break;
        }

        default:

            printf("Invalid choice!\n");
            break;
        }
    }

cleanup:

    close(sockfd);

    return exit_code;
}
