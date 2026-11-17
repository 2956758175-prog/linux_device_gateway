
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdint.h>

#include "protocol.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080
#define DEVICE_ID 3001

#define RESPONSE_LEN (PROTOCOL_HEADER_LEN + 1 + CRC16_LEN)

int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = send(fd, data + total,
                         len - total, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("send");
            return -1;
        }

        if (n == 0)
            return -1;

        total += (size_t)n;
    }

    return 0;
}

int recv_all(int fd, unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = recv(fd, data + total, len - total, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

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

int main(void)
{
    int sockfd;
    struct sockaddr_in server_addr;

    unsigned char frame[256];
    unsigned char response[RESPONSE_LEN];

    const unsigned char payload[] = "TCP_SPLIT_TEST";

    ProtocolHeader header;

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = CMD_DATA;
    header.device_id = DEVICE_ID;
    header.data_len = sizeof(payload) - 1;

    /* 组装协议头 */
    if (encode_header(frame, &header) < 0) {
        printf("encode_header failed.\n");
        return EXIT_FAILURE;
    }

    /* 组装 Payload */
    memcpy(frame + PROTOCOL_HEADER_LEN,
           payload, sizeof(payload) - 1);

    size_t crc_offset =
        PROTOCOL_HEADER_LEN + sizeof(payload) - 1;

    /* 计算 CRC */
    uint16_t crc = crc16_ccitt(frame, crc_offset);

    frame[crc_offset] = (unsigned char)(crc >> 8);
    frame[crc_offset + 1] = (unsigned char)(crc & 0xFF);

    size_t frame_len = crc_offset + CRC16_LEN;

    printf("Complete frame length: %zu bytes\n", frame_len);

    /* 创建 Socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP,
                  &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* 连接服务端 */
    if (connect(sockfd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to server.\n");

        /*
     * 先注册设备
     */
    printf("\nRegistering device...\n");

    unsigned char register_frame[
        PROTOCOL_HEADER_LEN + CRC16_LEN
    ];

    ProtocolHeader register_header;

    register_header.header = FRAME_HEADER;
    register_header.version = PROTOCOL_VERSION;
    register_header.command = CMD_REGISTER;
    register_header.device_id = DEVICE_ID;
    register_header.data_len = 0;

    if (encode_header(register_frame, &register_header) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint16_t register_crc =
        crc16_ccitt(register_frame, PROTOCOL_HEADER_LEN);

    register_frame[PROTOCOL_HEADER_LEN] =
        (unsigned char)(register_crc >> 8);

    register_frame[PROTOCOL_HEADER_LEN + 1] =
        (unsigned char)(register_crc & 0xFF);

    if (send_all(sockfd, register_frame, sizeof(register_frame)) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* 接收注册响应 */
    unsigned char register_response[RESPONSE_LEN];

    if (recv_all(sockfd, register_response, RESPONSE_LEN) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    ProtocolHeader register_response_header;

    if (decode_header(register_response,
                      &register_response_header) < 0 ||
        register_response_header.command != CMD_RESPONSE ||
        register_response_header.device_id != DEVICE_ID ||
        register_response_header.data_len != 1) {
        printf("Invalid registration response.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint16_t register_received_crc =
        ((uint16_t)register_response[PROTOCOL_HEADER_LEN + 1] << 8) |
        register_response[PROTOCOL_HEADER_LEN + 2];

    uint16_t register_calculated_crc =
        crc16_ccitt(register_response, PROTOCOL_HEADER_LEN + 1);

    if (register_received_crc != register_calculated_crc) {
        printf("Registration response CRC error.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint8_t register_status =
        register_response[PROTOCOL_HEADER_LEN];

    if (register_status != STATUS_SUCCESS &&
        register_status != STATUS_ALREADY_ONLINE) {
        printf("Device registration failed: 0x%02X\n",
               register_status);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Device registered successfully!\n");

    /*
     * 第一次发送：只发送前 5 个字节
     */
    size_t first_len = 5;

    printf("\nSending first part: %zu bytes\n", first_len);

    if (send_all(sockfd, frame, first_len) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("First part sent.\n");

    /*
     * 等待 1 秒，让服务端有机会先收到不完整数据
     */
    printf("Waiting 1 second...\n");
    sleep(1);

    /*
     * 第二次发送：发送剩余字节
     */
    size_t second_len = frame_len - first_len;

    printf("\nSending remaining part: %zu bytes\n", second_len);

    if (send_all(sockfd, frame + first_len, second_len) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Second part sent.\n");

    /* 接收服务端响应 */
    if (recv_all(sockfd, response, RESPONSE_LEN) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* 校验响应 */
    ProtocolHeader response_header;

    if (decode_header(response, &response_header) < 0 ||
        response_header.header != FRAME_HEADER ||
        response_header.version != PROTOCOL_VERSION ||
        response_header.command != CMD_RESPONSE ||
        response_header.device_id != DEVICE_ID ||
        response_header.data_len != 1) {
        printf("Invalid response header.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint16_t received_crc =
        ((uint16_t)response[PROTOCOL_HEADER_LEN + 1] << 8) |
        response[PROTOCOL_HEADER_LEN + 2];

    uint16_t calculated_crc =
        crc16_ccitt(response, PROTOCOL_HEADER_LEN + 1);

    if (received_crc != calculated_crc) {
        printf("Response CRC error.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint8_t status = response[PROTOCOL_HEADER_LEN];

    printf("\n========== Response ==========\n");
    printf("Device ID: %u\n", response_header.device_id);
    printf("Status: 0x%02X\n", status);

    if (status == STATUS_SUCCESS ||
        status == STATUS_ALREADY_ONLINE) {
        printf("TCP split test passed!\n");
    } else {
        printf("Server returned an error status.\n");
    }

    printf("==============================\n");

    close(sockfd);

    return EXIT_SUCCESS;
}

