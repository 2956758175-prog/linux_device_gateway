#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "protocol.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define DEVICE_ID 1002

#define INPUT_SIZE 1024

/* 确保所有数据都发送出去 */
int send_all(int sockfd, const unsigned char *buf, size_t len)
{
    size_t total = 0;

    while (total < len) {

        ssize_t n = send(sockfd,
                         buf + total,
                         len - total,
                         MSG_NOSIGNAL);

        if (n < 0) {

            /* 被信号中断时重新发送 */
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
 * 接收并解析服务端响应帧
 */
int receive_response(int sockfd, uint32_t expected_device_id)
{
    unsigned char frame[PROTOCOL_HEADER_LEN + 1 + CRC16_LEN];
    ProtocolHeader header;

    /* 响应帧固定为 15 字节 */
    if (recv_all(sockfd, frame, sizeof(frame)) < 0) {
        return -1;
    }

    /* 解析协议头 */
    if (decode_header(frame, &header) < 0) {
        printf("Invalid response header.\n");
        return -1;
    }

    /* 检查协议头 */
    if (header.header != FRAME_HEADER ||
        header.version != PROTOCOL_VERSION ||
        header.command != CMD_RESPONSE ||
        header.device_id != expected_device_id ||
        header.data_len != 1) {
        printf("Invalid response frame.\n");
        return -1;
    }

    /* 获取响应状态码 */
    uint8_t status = frame[PROTOCOL_HEADER_LEN];

    /* 计算 CRC */
    uint16_t received_crc =
        ((uint16_t)frame[PROTOCOL_HEADER_LEN + 1] << 8) |
        frame[PROTOCOL_HEADER_LEN + 2];

    uint16_t calculated_crc =
        crc16_ccitt(frame, PROTOCOL_HEADER_LEN + 1);

    if (received_crc != calculated_crc) {
        printf("Response CRC check failed!\n");
        return -1;
    }

    printf("\n========== Server Response ==========\n");
    printf("Device ID: %u\n", header.device_id);
    printf("Command: 0x%02X\n", header.command);
    printf("Status: 0x%02X\n", status);

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

    printf("=====================================\n");

    return 0;
}

/* 组装并发送协议帧 */
int send_protocol_frame(int sockfd,
                        uint8_t command,
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

    /* 填充协议头 */
    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = command;
    header.device_id = DEVICE_ID;
    header.data_len = payload_len;

    /* 编码协议头 */
    if (encode_header(frame, &header) < 0) {
        printf("Encode header failed!\n");
        return -1;
    }

    /* 拷贝 Payload */
    if (payload_len > 0) {
        memcpy(frame + PROTOCOL_HEADER_LEN,
               payload,
               payload_len);
    }

    /* 计算 CRC16 */
    size_t crc_offset = PROTOCOL_HEADER_LEN + payload_len;

    uint16_t crc = crc16_ccitt(frame, crc_offset);

    frame[crc_offset] = (unsigned char)(crc >> 8);
    frame[crc_offset + 1] = (unsigned char)(crc & 0xFF);

    /* 计算完整帧长度 */
    size_t frame_len = crc_offset + CRC16_LEN;

    /* 发送完整协议帧 */
    if (send_all(sockfd, frame, frame_len) < 0) {
    return -1;
    }

    printf("Frame sent successfully.\n");
    printf("Device ID: %u\n", DEVICE_ID);
    printf("Command: 0x%02X\n", command);
    printf("Payload Length: %u\n", payload_len);

    /* 等待并处理服务端响应 */
    if (receive_response(sockfd, DEVICE_ID) < 0) {
    return -1;
    }

    return 0;
}

int main(void)
{
    int sockfd;

    int exit_code = EXIT_SUCCESS;

    struct sockaddr_in server_addr;

    char input[INPUT_SIZE];

    /* 1. 创建 Socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* 2. 初始化服务器地址 */
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

    /* 3. 连接服务器 */
    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {

        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to server.\n");
    printf("Device ID: %u\n", DEVICE_ID);

    /* 4. 命令交互循环 */
    while (1) {

        int choice;

        printf("\n========== Device Client ==========\n");
        printf("1. Register Device\n");
        printf("2. Send Heartbeat\n");
        printf("3. Send Data\n");
        printf("4. Exit\n");
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

            /* 去掉输入末尾的换行符 */
            input[strcspn(input, "\n")] = '\0';

            if (send_protocol_frame(sockfd,
                                    CMD_DATA,
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

        default:

            printf("Invalid choice!\n");
            break;
        }
    }

cleanup:

    close(sockfd);

    return exit_code;
}
