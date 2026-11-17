
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
#define DEVICE_ID 2001

#define RESPONSE_LEN (PROTOCOL_HEADER_LEN + 1 + CRC16_LEN)

/* 确保完整发送 */
int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = send(fd, data + total, len - total, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("send");
            return -1;
        }

        if (n == 0) {
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/* 确保接收指定长度 */
int recv_all(int fd, unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = recv(fd, data + total, len - total, 0);

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
 * 组装一帧协议数据，并追加到发送缓冲区
 */
int append_frame(unsigned char *buffer,
                 size_t buffer_size,
                 size_t *offset,
                 uint8_t command,
                 const unsigned char *payload,
                 uint32_t payload_len)
{
    if (payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }

    if (payload_len > 0 && payload == NULL) {
        return -1;
    }

    size_t frame_len =
        PROTOCOL_HEADER_LEN + payload_len + CRC16_LEN;

    if (*offset > buffer_size ||
        frame_len > buffer_size - *offset) {
        return -1;
    }

    unsigned char *frame = buffer + *offset;

    ProtocolHeader header;

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = command;
    header.device_id = DEVICE_ID;
    header.data_len = payload_len;

    if (encode_header(frame, &header) < 0) {
        return -1;
    }

    if (payload_len > 0) {
        memcpy(frame + PROTOCOL_HEADER_LEN,
               payload,
               payload_len);
    }

    size_t crc_offset = PROTOCOL_HEADER_LEN + payload_len;

    uint16_t crc = crc16_ccitt(frame, crc_offset);

    frame[crc_offset] = (unsigned char)(crc >> 8);
    frame[crc_offset + 1] = (unsigned char)(crc & 0xFF);

    *offset += frame_len;

    return 0;
}

/* 检查服务端响应 */
int check_response(const unsigned char *frame, int index)
{
    ProtocolHeader header;

    if (decode_header(frame, &header) < 0) {
        printf("Response %d: invalid header.\n", index);
        return -1;
    }

    if (header.header != FRAME_HEADER ||
        header.version != PROTOCOL_VERSION ||
        header.command != CMD_RESPONSE ||
        header.device_id != DEVICE_ID ||
        header.data_len != 1) {
        printf("Response %d: invalid frame.\n", index);
        return -1;
    }

    uint16_t received_crc =
        ((uint16_t)frame[PROTOCOL_HEADER_LEN + 1] << 8) |
        frame[PROTOCOL_HEADER_LEN + 2];

    uint16_t calculated_crc =
        crc16_ccitt(frame, PROTOCOL_HEADER_LEN + 1);

    if (received_crc != calculated_crc) {
        printf("Response %d: CRC error.\n", index);
        return -1;
    }

    uint8_t status = frame[PROTOCOL_HEADER_LEN];

    printf("Response %d: Device ID=%u, Status=0x%02X\n",
           index,
           header.device_id,
           status);

    if (status != STATUS_SUCCESS &&
        status != STATUS_ALREADY_ONLINE) {
        printf("Response %d: operation failed.\n", index);
        return -1;
    }

    return 0;
}

int main(void)
{
    int sockfd;

    struct sockaddr_in server_addr;

    unsigned char send_buffer[4096];
    unsigned char recv_buffer[3 * RESPONSE_LEN];

    size_t send_len = 0;

    const unsigned char data[] = "TCP_COALESCING_TEST";

    /* 创建 Socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
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
                sizeof(server_addr)) == -1) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to server.\n");
    printf("Device ID: %u\n", DEVICE_ID);

    /*
     * 连续组装三帧
     */

    /* 第一帧：设备注册 */
    if (append_frame(send_buffer,
                     sizeof(send_buffer),
                     &send_len,
                     CMD_REGISTER,
                     NULL,
                     0) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* 第二帧：心跳 */
    if (append_frame(send_buffer,
                     sizeof(send_buffer),
                     &send_len,
                     CMD_HEARTBEAT,
                     NULL,
                     0) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* 第三帧：数据上报 */
    if (append_frame(send_buffer,
                     sizeof(send_buffer),
                     &send_len,
                     CMD_DATA,
                     data,
                     sizeof(data) - 1) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("\nPrepared 3 protocol frames.\n");
    printf("Total send length: %zu bytes\n", send_len);

    /*
     * 一次 send_all 发送三帧连续数据
     */
    printf("\nSending all frames in one continuous stream...\n");

    if (send_all(sockfd, send_buffer, send_len) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("All frames sent successfully.\n");

    /*
     * 三帧分别对应三个固定长度响应
     */
    if (recv_all(sockfd,
                 recv_buffer,
                 sizeof(recv_buffer)) < 0) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("\n========== Server Responses ==========\n");

    for (int i = 0; i < 3; i++) {
        if (check_response(
                recv_buffer + i * RESPONSE_LEN,
                i + 1) < 0) {
            close(sockfd);
            return EXIT_FAILURE;
        }
    }

    printf("======================================\n");
    printf("TCP coalescing test passed!\n");

    close(sockfd);

    return EXIT_SUCCESS;
}

