
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

#include "protocol.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080

#define MAX_LOAD_CLIENTS 16
#define DEFAULT_CLIENTS 4
#define DEFAULT_LOOPS 10

#define FIRST_DEVICE_ID 2001

#define RESPONSE_PAYLOAD_LEN 1

#define RESPONSE_FRAME_LEN \
    (PROTOCOL_HEADER_LEN + RESPONSE_PAYLOAD_LEN + CRC16_LEN)

#define MAX_FRAME_SIZE \
    (PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN)


/* 发送完整数据 */
static int send_all(int fd, const unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = send(fd,
                         data + total,
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
            fprintf(stderr, "send returned 0.\n");
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}


/* 接收指定长度的数据 */
static int recv_all(int fd, unsigned char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = recv(fd,
                         data + total,
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
            fprintf(stderr, "Server closed connection.\n");
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}


/* 构造协议帧 */
static int append_frame(unsigned char *buf,
                        size_t buf_size,
                        size_t *frame_len,
                        uint8_t command,
                        uint32_t device_id,
                        const unsigned char *payload,
                        uint32_t payload_len)
{
    ProtocolHeader header;
    uint16_t crc;
    size_t total_len;

    if (buf == NULL || frame_len == NULL) {
        return -1;
    }

    if (payload_len > MAX_PAYLOAD_LEN) {
        return -1;
    }

    if (payload_len > 0 && payload == NULL) {
        return -1;
    }

    total_len = PROTOCOL_HEADER_LEN + payload_len + CRC16_LEN;

    if (buf_size < total_len) {
        return -1;
    }

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = command;
    header.device_id = device_id;
    header.data_len = payload_len;

    /*
     * 注意：
     * encode_header 成功返回 PROTOCOL_HEADER_LEN，
     * 失败返回 -1。
     */
    if (encode_header(buf, &header) < 0) {
        fprintf(stderr, "encode_header failed.\n");
        return -1;
    }

    if (payload_len > 0) {
        memcpy(buf + PROTOCOL_HEADER_LEN,
               payload,
               payload_len);
    }

    /* 计算 CRC16 */
    crc = crc16_ccitt(buf, PROTOCOL_HEADER_LEN + payload_len);

    /* CRC 高字节在前 */
    buf[PROTOCOL_HEADER_LEN + payload_len] =
        (unsigned char)(crc >> 8);

    buf[PROTOCOL_HEADER_LEN + payload_len + 1] =
        (unsigned char)(crc & 0xFF);

    *frame_len = total_len;

    return 0;
}


/* 检查服务端响应 */
static int check_response(int fd,
                          uint32_t expected_device_id,
                          int allow_already_online)
{
    unsigned char response[RESPONSE_FRAME_LEN];

    ProtocolHeader header;

    uint16_t received_crc;
    uint16_t calculated_crc;

    unsigned char status;

    if (recv_all(fd, response, sizeof(response)) < 0) {
        return -1;
    }

    /*
     * decode_header 成功返回 PROTOCOL_HEADER_LEN，
     * 失败返回 -1。
     */
    if (decode_header(response, &header) < 0) {
        fprintf(stderr, "decode_header failed.\n");
        return -1;
    }

    /* 检查帧头 */
    if (header.header != FRAME_HEADER) {
        fprintf(stderr, "Invalid response header.\n");
        return -1;
    }

    /* 检查协议版本 */
    if (header.version != PROTOCOL_VERSION) {
        fprintf(stderr, "Invalid protocol version.\n");
        return -1;
    }

    /* 检查响应命令 */
    if (header.command != CMD_RESPONSE) {
        fprintf(stderr,
                "Invalid response command: 0x%02X\n",
                header.command);
        return -1;
    }

    /* 检查设备 ID */
    if (header.device_id != expected_device_id) {
        fprintf(stderr, "Device ID mismatch.\n");
        return -1;
    }

    /* 检查 Payload 长度 */
    if (header.data_len != RESPONSE_PAYLOAD_LEN) {
        fprintf(stderr, "Invalid response payload length.\n");
        return -1;
    }

    /* CRC 校验 */
    received_crc =
        ((uint16_t)response[RESPONSE_FRAME_LEN - 2] << 8) |
        response[RESPONSE_FRAME_LEN - 1];

    calculated_crc =
        crc16_ccitt(response, RESPONSE_FRAME_LEN - CRC16_LEN);

    if (received_crc != calculated_crc) {
        fprintf(stderr, "Response CRC error.\n");
        return -1;
    }

    /* 获取响应状态 */
    status = response[PROTOCOL_HEADER_LEN];

    if (status == STATUS_SUCCESS) {
        return 0;
    }

    if (allow_already_online &&
        status == STATUS_ALREADY_ONLINE) {
        return 0;
    }

    fprintf(stderr,
            "Server rejected request. Status: 0x%02X\n",
            status);

    return -1;
}


/* 发送请求并等待服务端响应 */
static int send_request(int fd,
                        uint32_t device_id,
                        uint8_t command,
                        const unsigned char *payload,
                        uint32_t payload_len,
                        int allow_already_online)
{
    unsigned char frame[MAX_FRAME_SIZE];

    size_t frame_len = 0;

    if (append_frame(frame,
                     sizeof(frame),
                     &frame_len,
                     command,
                     device_id,
                     payload,
                     payload_len) < 0) {
        fprintf(stderr, "Build frame failed.\n");
        return -1;
    }

    if (send_all(fd, frame, frame_len) < 0) {
        return -1;
    }

    if (check_response(fd,
                       device_id,
                       allow_already_online) < 0) {
        return -1;
    }

    return 0;
}


/* 建立 TCP 连接 */
static int connect_server(void)
{
    int fd;

    struct sockaddr_in server_addr;
    struct timeval timeout;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置接收和发送超时 */
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(fd);
        return -1;
    }

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   sizeof(timeout)) < 0) {
        perror("setsockopt SO_SNDTIMEO");
        close(fd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET,
                  SERVER_IP,
                  &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd,
                (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}


/* 单个子进程执行的客户端任务 */
static int run_client(uint32_t device_id, int loops)
{
    int fd;
    int i;

    unsigned char data_payload[128];

    int payload_len;

    printf("[Client %u] Connecting to server...\n",
           device_id);

    fd = connect_server();

    if (fd < 0) {
        printf("[Client %u] Connection failed.\n",
               device_id);
        return -1;
    }

    printf("[Client %u] Connected successfully.\n",
           device_id);

    /* 第一步：注册设备 */
    printf("[Client %u] Registering...\n",
           device_id);

    if (send_request(fd,
                     device_id,
                     CMD_REGISTER,
                     NULL,
                     0,
                     1) < 0) {
        printf("[Client %u] Registration failed.\n",
               device_id);

        close(fd);
        return -1;
    }

    printf("[Client %u] Registration successful.\n",
           device_id);

    /* 第二步：循环发送心跳和数据 */
    for (i = 0; i < loops; i++) {

        /* 发送心跳 */
        if (send_request(fd,
                         device_id,
                         CMD_HEARTBEAT,
                         NULL,
                         0,
                         0) < 0) {
            printf("[Client %u] Heartbeat failed at round %d.\n",
                   device_id,
                   i + 1);

            close(fd);
            return -1;
        }

        /* 构造数据上报 Payload */
        payload_len = snprintf((char *)data_payload,
                               sizeof(data_payload),
                               "LOAD_TEST device=%u round=%d",
                               device_id,
                               i + 1);

        if (payload_len < 0 ||
            (size_t)payload_len >= sizeof(data_payload)) {
            fprintf(stderr, "Build data payload failed.\n");

            close(fd);
            return -1;
        }

        /* 发送数据 */
        if (send_request(fd,
                         device_id,
                         CMD_DATA,
                         data_payload,
                         (uint32_t)payload_len,
                         0) < 0) {
            printf("[Client %u] Data upload failed at round %d.\n",
                   device_id,
                   i + 1);

            close(fd);
            return -1;
        }
    }

    printf("[Client %u] Test completed: "
           "%d heartbeats, %d data uploads.\n",
           device_id,
           loops,
           loops);

    close(fd);

    return 0;
}


/* 解析正整数参数 */
static int parse_positive_int(const char *str, int *value)
{
    char *endptr;
    long number;

    if (str == NULL || value == NULL) {
        return -1;
    }

    errno = 0;

    number = strtol(str, &endptr, 10);

    if (errno != 0 ||
        endptr == str ||
        *endptr != '\0' ||
        number <= 0 ||
        number > 1000000) {
        return -1;
    }

    *value = (int)number;

    return 0;
}


/* 主函数 */
int main(int argc, char *argv[])
{
    int client_count = DEFAULT_CLIENTS;
    int loops = DEFAULT_LOOPS;

    pid_t child_pids[MAX_LOAD_CLIENTS];

    int launched = 0;

    int success_count = 0;
    int failed_count = 0;

    int i;
    int status;

    /* 解析客户端数量 */
    if (argc >= 2) {
        if (parse_positive_int(argv[1], &client_count) < 0) {
            fprintf(stderr, "Invalid client count.\n");
            return EXIT_FAILURE;
        }
    }

    /* 解析循环次数 */
    if (argc >= 3) {
        if (parse_positive_int(argv[2], &loops) < 0) {
            fprintf(stderr, "Invalid loop count.\n");
            return EXIT_FAILURE;
        }
    }

    if (argc > 3) {
        fprintf(stderr,
                "Usage: %s [client_count] [loops]\n",
                argv[0]);

        return EXIT_FAILURE;
    }

    if (client_count > MAX_LOAD_CLIENTS) {
        fprintf(stderr,
                "Maximum supported clients: %d\n",
                MAX_LOAD_CLIENTS);

        return EXIT_FAILURE;
    }

    printf("========== Multi-Client Load Test ==========\n");

    printf("Server: %s:%d\n",
           SERVER_IP,
           SERVER_PORT);

    printf("Client count: %d\n",
           client_count);

    printf("Loops per client: %d\n",
           loops);

    printf("Device ID range: %d - %d\n",
           FIRST_DEVICE_ID,
           FIRST_DEVICE_ID + client_count - 1);

    printf("============================================\n");

    fflush(stdout);

    /* 创建多个客户端子进程 */
    for (i = 0; i < client_count; i++) {

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) {

            uint32_t device_id =
                FIRST_DEVICE_ID + (uint32_t)i;

            int ret = run_client(device_id, loops);

            fflush(stdout);
            fflush(stderr);

            _exit(ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
        }

        child_pids[launched++] = pid;
    }

    /* 父进程等待所有子进程 */
    for (i = 0; i < launched; i++) {

        pid_t ret;

        do {
            ret = waitpid(child_pids[i], &status, 0);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            perror("waitpid");
            failed_count++;
            continue;
        }

        if (WIFEXITED(status) &&
            WEXITSTATUS(status) == EXIT_SUCCESS) {
            success_count++;
        } else {
            failed_count++;
        }
    }

    /* 输出最终统计 */
    printf("\n========== Load Test Result ==========\n");

    printf("Requested clients: %d\n",
           client_count);

    printf("Launched clients:  %d\n",
           launched);

    printf("Successful:        %d\n",
           success_count);

    printf("Failed:            %d\n",
           failed_count);

    printf("Loops per client:  %d\n",
           loops);

    printf("Expected heartbeats: %d\n",
           success_count * loops);

    printf("Expected data uploads: %d\n",
           success_count * loops);

    printf("======================================\n");

    if (launched != client_count || failed_count > 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
