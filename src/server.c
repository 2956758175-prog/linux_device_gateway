#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>

#include "protocol_parser.h"
#include "device_manager.h"
#include "logger.h"

#define SERVER_PORT 8080
#define RECV_SIZE 512
#define MAX_CLIENTS 16
#define MAX_EVENTS 32
#define SEND_BUFFER_SIZE 8192

/*
 * 每个客户端独立的发送缓冲区
 */
typedef struct {
    unsigned char buffer[SEND_BUFFER_SIZE];
    size_t len;
    size_t offset;
} ClientSendBuffer;

/*
 * 服务器运行状态统计
 */
typedef struct {
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t total_disconnections;

    uint64_t total_rx_bytes;
    uint64_t total_tx_bytes;

    uint64_t total_frames;

    uint64_t register_requests;
    uint64_t heartbeat_requests;
    uint64_t data_requests;
    uint64_t query_requests;
} ServerStats;

static ServerStats g_stats = {0};
static time_t g_stats_last_print = 0;

/*
 * 服务器退出标志
 *
 * volatile：防止编译器将变量访问优化掉
 * sig_atomic_t：保证信号处理函数中的读写操作是原子的
 */
static volatile sig_atomic_t g_stop = 0;

/*
 * 信号处理函数
 *
 * 这里只设置标志位，不执行复杂操作
 */
static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/*
 * 注册 SIGINT 和 SIGTERM
 */
static int setup_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        return -1;
    }

    return 0;
}


/*
 * 设置文件描述符为非阻塞模式
 */
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}


/*
 * 打印服务器运行统计信息
 */
void print_server_stats(void)
{
    printf("\n========== Server Statistics ==========\n");

    printf("Total Connections:       %llu\n",
           (unsigned long long)g_stats.total_connections);

    printf("Active Connections:      %llu\n",
           (unsigned long long)g_stats.active_connections);

    printf("Total Disconnections:    %llu\n",
           (unsigned long long)g_stats.total_disconnections);

    printf("\n");

    printf("RX Bytes:                %llu\n",
           (unsigned long long)g_stats.total_rx_bytes);

    printf("TX Bytes:                %llu\n",
           (unsigned long long)g_stats.total_tx_bytes);

    printf("Total Frames:            %llu\n",
           (unsigned long long)g_stats.total_frames);

    printf("\n");

    printf("Register Requests:       %llu\n",
           (unsigned long long)g_stats.register_requests);

    printf("Heartbeat Requests:      %llu\n",
           (unsigned long long)g_stats.heartbeat_requests);

    printf("Data Requests:           %llu\n",
           (unsigned long long)g_stats.data_requests);

    printf("Query Requests:          %llu\n",
           (unsigned long long)g_stats.query_requests);

    printf("=======================================\n");
}


/*
 * 每隔 10 秒打印一次统计信息
 */
void check_server_stats(void)
{
    time_t now = time(NULL);

    if (g_stats_last_print == 0) {
        g_stats_last_print = now;
        return;
    }

    if (difftime(now, g_stats_last_print) >= 10) {
        print_server_stats();
        g_stats_last_print = now;
    }
}


/*
 * 打印设备信息
 */
void print_device_info(const DeviceInfo *device)
{
    if (device == NULL) {
        return;
    }

    printf("Device ID: %u\n", device->device_id);
    printf("Online: %s\n", device->online ? "YES" : "NO");
    printf("Last Seen: %ld\n", (long)device->last_seen);
}


/*
 * 查找客户端对应的数组槽位
 */
int find_client_slot(const int client_fds[], int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == fd) {
            return i;
        }
    }

    return -1;
}


/*
 * 查找空闲客户端槽位
 */
int find_free_slot(const int client_fds[])
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == -1) {
            return i;
        }
    }

    return -1;
}


/*
 * 根据发送缓冲区状态更新客户端 epoll 事件
 */
int update_client_events(int epfd, int client_fd, int want_write)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));

    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

    if (want_write) {
        ev.events |= EPOLLOUT;
    }

    ev.data.fd = client_fd;

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev) == -1) {
        perror("epoll_ctl MOD client");

        log_write(LOG_LEVEL_ERROR,
                  "epoll_ctl MOD failed: FD=%d, Error=%s",
                  client_fd,
                  strerror(errno));

        return -1;
    }

    return 0;
}


/*
 * 将响应数据放入客户端发送缓冲区
 */
int queue_client_data(ClientSendBuffer *send_buffer,
                      const unsigned char *data,
                      size_t len)
{
    if (send_buffer == NULL || data == NULL || len == 0) {
        return -1;
    }

    if (send_buffer->offset > send_buffer->len) {
        return -1;
    }

    size_t pending = send_buffer->len - send_buffer->offset;

    /*
     * 尾部空间不足时，移动未发送的数据
     */
    if (send_buffer->offset > 0 &&
        SEND_BUFFER_SIZE - send_buffer->len < len) {

        memmove(send_buffer->buffer,
                send_buffer->buffer + send_buffer->offset,
                pending);

        send_buffer->offset = 0;
        send_buffer->len = pending;
    }

    if (SEND_BUFFER_SIZE - send_buffer->len < len) {
        printf("Client send buffer is full.\n");

        log_write(LOG_LEVEL_ERROR,
                  "Client send buffer is full.");

        return -1;
    }

    memcpy(send_buffer->buffer + send_buffer->len,
           data,
           len);

    send_buffer->len += len;

    return 0;
}


/*
 * 尝试发送客户端缓冲区中的全部数据
 */
int flush_client_send_buffer(int epfd,
                             int client_fd,
                             ClientSendBuffer *send_buffer)
{
    while (send_buffer->offset < send_buffer->len) {

        ssize_t n = send(
            client_fd,
            send_buffer->buffer + send_buffer->offset,
            send_buffer->len - send_buffer->offset,
            MSG_NOSIGNAL
        );

        if (n > 0) {
            g_stats.total_tx_bytes += (uint64_t)n;

            send_buffer->offset += (size_t)n;
            continue;
        }

        if (n == 0) {
            printf("send returned 0.\n");

            log_write(LOG_LEVEL_ERROR,
                      "send returned 0: FD=%d",
                      client_fd);

            return -1;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {

            if (update_client_events(epfd, client_fd, 1) == -1) {
                return -1;
            }

            return 0;
        }

        int saved_errno = errno;

        perror("send");

        log_write(LOG_LEVEL_ERROR,
                  "send failed: FD=%d, Error=%s",
                  client_fd,
                  strerror(saved_errno));

        return -1;
    }

    send_buffer->len = 0;
    send_buffer->offset = 0;

    if (update_client_events(epfd, client_fd, 0) == -1) {
        return -1;
    }

    return 0;
}


/*
 * 将 uint64_t 编码为 8 字节大端序
 */
static void encode_u64(unsigned char *buf, uint64_t value)
{
    for (int i = 7; i >= 0; i--) {
        buf[i] = (unsigned char)(value & 0xFF);
        value >>= 8;
    }
}


/*
 * 发送可变长度响应帧
 */
int send_response_payload(int epfd,
                          int client_fd,
                          ClientSendBuffer *send_buffer,
                          uint32_t device_id,
                          const unsigned char *payload,
                          uint32_t payload_len)
{
    unsigned char frame[
        PROTOCOL_HEADER_LEN + MAX_PAYLOAD_LEN + CRC16_LEN
    ];

    ProtocolHeader header;

    if (payload_len > MAX_PAYLOAD_LEN) {
        printf("Response payload too large.\n");

        log_write(LOG_LEVEL_ERROR,
                  "Response payload too large: Length=%u",
                  payload_len);

        return -1;
    }

    if (payload_len > 0 && payload == NULL) {
        return -1;
    }

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = CMD_RESPONSE;
    header.device_id = device_id;
    header.data_len = payload_len;

    if (encode_header(frame, &header) < 0) {
        fprintf(stderr, "Failed to encode response header.\n");

        log_write(LOG_LEVEL_ERROR,
                  "Failed to encode response header.");

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

    size_t frame_len = crc_offset + CRC16_LEN;

    if (queue_client_data(send_buffer, frame, frame_len) < 0) {
        return -1;
    }

    if (flush_client_send_buffer(
            epfd,
            client_fd,
            send_buffer) < 0) {
        return -1;
    }

    printf("Response queued: Device ID=%u, Payload Length=%u\n",
           device_id,
           payload_len);

    return 0;
}


/*
 * 普通业务响应：Payload 只有一个状态码
 */
int send_response(int epfd,
                  int client_fd,
                  ClientSendBuffer *send_buffer,
                  uint32_t device_id,
                  uint8_t status)
{
    return send_response_payload(
        epfd,
        client_fd,
        send_buffer,
        device_id,
        &status,
        1
    );
}


/*
 * 断开指定客户端并释放资源
 */
void disconnect_client(int epfd,
                       int client_fds[],
                       ProtocolParser parsers[],
                       ClientSendBuffer send_buffers[],
                       int index)
{
    if (index < 0 || index >= MAX_CLIENTS) {
        return;
    }

    if (client_fds[index] < 0) {
        return;
    }

    int fd = client_fds[index];

    g_stats.total_disconnections++;

    if (g_stats.active_connections > 0) {
        g_stats.active_connections--;
    }

    printf("\nClient disconnected: fd=%d\n", fd);

    log_write(LOG_LEVEL_INFO,
              "Client disconnected: FD=%d, Slot=%d",
              fd,
              index);

    if (parsers[index].len > 0) {
        printf("Warning: incomplete frame discarded (%zu bytes).\n",
               parsers[index].len);

        log_write(LOG_LEVEL_WARN,
                  "Incomplete frame discarded: FD=%d, Bytes=%zu",
                  fd,
                  parsers[index].len);
    }

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        if (errno != ENOENT && errno != EBADF) {
            int saved_errno = errno;

            perror("epoll_ctl DEL");

            log_write(LOG_LEVEL_ERROR,
                      "epoll_ctl DEL failed: FD=%d, Error=%s",
                      fd,
                      strerror(saved_errno));
        }
    }

    close(fd);

    client_fds[index] = -1;

    protocol_parser_init(&parsers[index]);

    memset(&send_buffers[index], 0, sizeof(send_buffers[index]));

    printf("Client slot %d released.\n", index);
}


/*
 * 处理设备业务命令
 */
int process_frame(int epfd,
                  DeviceManager *manager,
                  const ProtocolFrame *frame,
                  int client_fd,
                  ClientSendBuffer *send_buffer)
{
    uint8_t status = STATUS_FAILED;

    unsigned char response_payload[
        2 + MAX_DEVICES * 13
    ];

    uint32_t response_len = 1;

    printf("\n========== Process Command ==========\n");
    printf("Device ID: %u ", frame->header.device_id);
    printf("Command: 0x%02X\n", frame->header.command);

    log_write(LOG_LEVEL_INFO,
              "Received command: Device ID=%u, Command=0x%02X, Payload Length=%u",
              frame->header.device_id,
              frame->header.command,
              frame->header.data_len);

    switch (frame->header.command) {

    case CMD_REGISTER:
    {
        g_stats.register_requests++;

        printf("Command Type: DEVICE REGISTER\n");

        int ret = device_manager_register(
            manager,
            frame->header.device_id
        );

        if (ret == 1) {
            printf("New device registered successfully.\n");

            log_write(LOG_LEVEL_INFO,
                      "New device registered: ID=%u",
                      frame->header.device_id);

            status = STATUS_SUCCESS;

        } else if (ret == 2) {
            printf("Device reconnected successfully.\n");

            log_write(LOG_LEVEL_INFO,
                      "Device reconnected: ID=%u",
                      frame->header.device_id);

            status = STATUS_SUCCESS;

        } else if (ret == 0) {
            printf("Device is already online.\n");

            log_write(LOG_LEVEL_WARN,
                      "Device already online: ID=%u",
                      frame->header.device_id);

            status = STATUS_ALREADY_ONLINE;

        } else {
            printf("Device registration failed.\n");

            log_write(LOG_LEVEL_ERROR,
                      "Device registration failed: ID=%u",
                      frame->header.device_id);

            status = STATUS_FAILED;
        }

        break;
    }

    case CMD_HEARTBEAT:
    {
        g_stats.heartbeat_requests++;

        printf("Command Type: DEVICE HEARTBEAT\n");

        int ret = device_manager_heartbeat(
            manager,
            frame->header.device_id
        );

        if (ret == 0) {
            printf("Heartbeat received successfully.\n");
            status = STATUS_SUCCESS;
        } else {
            printf("Heartbeat rejected: device not registered or offline.\n");

            log_write(LOG_LEVEL_WARN,
                      "Heartbeat rejected: Device ID=%u",
                      frame->header.device_id);

            status = STATUS_NOT_REGISTERED;
        }

        break;
    }

    case CMD_DATA:
    {
        g_stats.data_requests++;

        printf("Command Type: DEVICE DATA\n");

        int ret = device_manager_heartbeat(
            manager,
            frame->header.device_id
        );

        if (ret == 0) {
            printf("Device data received:\n");

            printf("%.*s\n",
                   (int)frame->header.data_len,
                   frame->payload);

            status = STATUS_SUCCESS;

        } else {
            printf("Data rejected: device not registered or offline.\n");

            log_write(LOG_LEVEL_WARN,
                      "Data rejected: Device ID=%u",
                      frame->header.device_id);

            status = STATUS_NOT_REGISTERED;
        }

        break;
    }

    case CMD_QUERY:
    {
        g_stats.query_requests++;

        printf("Command Type: DEVICE QUERY\n");

        if (frame->header.data_len != 0) {
            printf("Invalid query request payload.\n");

            log_write(LOG_LEVEL_WARN,
                      "Invalid query payload: Device ID=%u, Length=%u",
                      frame->header.device_id,
                      frame->header.data_len);

            status = STATUS_FAILED;
            break;
        }

        uint32_t target_id = frame->header.device_id;
        uint8_t count = 0;

        size_t offset = 2;

        if (target_id == 0) {

            printf("Querying all devices.\n");

            status = STATUS_SUCCESS;

            for (size_t i = 0; i < manager->count; i++) {

                const DeviceInfo *device = &manager->devices[i];

                uint32_t id = htonl(device->device_id);

                memcpy(response_payload + offset, &id, 4);
                offset += 4;

                response_payload[offset++] =
                    device->online ? 1 : 0;

                uint64_t last_seen =
                    device->last_seen > 0
                    ? (uint64_t)device->last_seen
                    : 0;

                encode_u64(response_payload + offset, last_seen);
                offset += 8;

                count++;
            }

        } else {

            printf("Querying device ID: %u\n", target_id);

            const DeviceInfo *device =
                device_manager_find(manager, target_id);

            if (device == NULL) {

                printf("Device not found.\n");
                status = STATUS_NOT_REGISTERED;

            } else {

                status = STATUS_SUCCESS;

                uint32_t id = htonl(device->device_id);

                memcpy(response_payload + offset, &id, 4);
                offset += 4;

                response_payload[offset++] =
                    device->online ? 1 : 0;

                uint64_t last_seen =
                    device->last_seen > 0
                    ? (uint64_t)device->last_seen
                    : 0;

                encode_u64(response_payload + offset, last_seen);
                offset += 8;

                count = 1;
            }
        }

        /*
         * 查询响应格式：
         * status(1) + count(1) + entries(count * 13)
         */
        response_payload[0] = status;
        response_payload[1] = count;

        response_len = (uint32_t)offset;

        printf("Query result: status=0x%02X, count=%u\n",
               status,
               count);

        log_write(LOG_LEVEL_INFO,
                  "Query completed: Target ID=%u, Status=0x%02X, Count=%u",
                  target_id,
                  status,
                  count);

        break;
    }

    default:
        printf("Unknown command.\n");

        log_write(LOG_LEVEL_WARN,
                  "Unknown command: Device ID=%u, Command=0x%02X",
                  frame->header.device_id,
                  frame->header.command);

        status = STATUS_FAILED;
        break;
    }

    printf("=====================================\n");

    if (frame->header.command == CMD_QUERY) {

        return send_response_payload(
            epfd,
            client_fd,
            send_buffer,
            frame->header.device_id,
            response_payload,
            response_len
        );
    }

    return send_response(
        epfd,
        client_fd,
        send_buffer,
        frame->header.device_id,
        status
    );
}


/*
 * 检查设备心跳超时
 */
void check_devices(DeviceManager *manager)
{
    int offline_count =
        device_manager_check_timeout(manager, HEARTBEAT_TIMEOUT);

    if (offline_count > 0) {

        printf("\n========== Device Timeout ==========\n");
        printf("%d device(s) went offline.\n", offline_count);

        log_write(LOG_LEVEL_WARN,
                  "%d device(s) went offline due to heartbeat timeout.",
                  offline_count);

        for (size_t i = 0; i < manager->count; i++) {

            const DeviceInfo *device = &manager->devices[i];

            if (!device->online) {
                print_device_info(device);

                log_write(LOG_LEVEL_WARN,
                          "Device offline: ID=%u, Last Seen=%ld",
                          device->device_id,
                          (long)device->last_seen);
            }
        }

        printf("====================================\n");
    }
}


/*
 * 接收并处理客户端数据
 *
 * ET 模式下必须循环 recv，
 * 直到返回 EAGAIN 或 EWOULDBLOCK。
 */
int handle_client_data(int epfd,
                       int client_fds[],
                       ProtocolParser parsers[],
                       ClientSendBuffer send_buffers[],
                       DeviceManager *manager,
                       int index,
                       int client_fd)
{
    unsigned char recv_buffer[RECV_SIZE];
    ProtocolFrame frame;

    while (1) {

        ssize_t n = recv(
            client_fd,
            recv_buffer,
            sizeof(recv_buffer),
            0
        );

        if (n == 0) {

            disconnect_client(
                epfd,
                client_fds,
                parsers,
                send_buffers,
                index
            );

            return -1;
        }

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }

            int saved_errno = errno;

            perror("recv");

            log_write(LOG_LEVEL_ERROR,
                      "recv failed: FD=%d, Error=%s",
                      client_fd,
                      strerror(saved_errno));

            disconnect_client(
                epfd,
                client_fds,
                parsers,
                send_buffers,
                index
            );

            return -1;
        }

        g_stats.total_rx_bytes += (uint64_t)n;

        printf("\nReceived %zd bytes from client fd=%d, slot=%d.\n",
               n,
               client_fd,
               index);

        if (protocol_parser_append(
                &parsers[index],
                recv_buffer,
                (size_t)n) < 0) {

            printf("Protocol parser buffer overflow!\n");

            log_write(LOG_LEVEL_ERROR,
                      "Protocol parser buffer overflow: FD=%d",
                      client_fd);

            disconnect_client(
                epfd,
                client_fds,
                parsers,
                send_buffers,
                index
            );

            return -1;
        }

        /*
         * 循环解析当前客户端缓存中的完整帧
         */
        while (1) {

            int parse_ret =
                protocol_parser_next(&parsers[index], &frame);

            if (parse_ret == 1) {

                g_stats.total_frames++;

                printf("\n========== Protocol Frame ==========\n");

                printf("Device ID: %u ", frame.header.device_id);
                printf("Command: 0x%02X ", frame.header.command);
                printf("Data Length: %u ", frame.header.data_len);

                printf("Payload: ");

                fwrite(
                    frame.payload,
                    1,
                    frame.header.data_len,
                    stdout
                );

                printf("\n====================================\n");

                if (process_frame(
                        epfd,
                        manager,
                        &frame,
                        client_fd,
                        &send_buffers[index]) < 0) {

                    disconnect_client(
                        epfd,
                        client_fds,
                        parsers,
                        send_buffers,
                        index
                    );

                    return -1;
                }

            } else if (parse_ret == 0) {

                break;

            } else {

                printf("Protocol parsing error!\n");

                log_write(LOG_LEVEL_ERROR,
                          "Protocol parsing error: FD=%d",
                          client_fd);

                disconnect_client(
                    epfd,
                    client_fds,
                    parsers,
                    send_buffers,
                    index
                );

                return -1;
            }
        }
    }
}


/*
 * 主函数
 */

int main(void)
{
    int server_fd = -1;
    int epfd = -1;
    int opt = 1;
    int exit_status = EXIT_FAILURE;

    int client_fds[MAX_CLIENTS];

    struct sockaddr_in server_addr;

    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];

    ProtocolParser parsers[MAX_CLIENTS];

    ClientSendBuffer send_buffers[MAX_CLIENTS];

    DeviceManager manager;

    /*
     * 1. 初始化客户端数组、解析器和发送缓冲区
     */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;

        protocol_parser_init(&parsers[i]);

        memset(&send_buffers[i], 0, sizeof(send_buffers[i]));
    }

    /*
     * 2. 初始化设备管理器
     */
    device_manager_init(&manager);

    /*
     * 3. 初始化日志模块
     */
    if (log_init("logs/server.log") == -1) {
        fprintf(stderr, "Failed to initialize logger.\n");
        return EXIT_FAILURE;
    }

    log_set_level(LOG_LEVEL_INFO);

    log_write(LOG_LEVEL_INFO, "Logger initialized.");

    /*
     * 4. 注册 SIGINT 和 SIGTERM
     */
    if (setup_signal_handlers() == -1) {
        log_write(LOG_LEVEL_ERROR,
                  "Failed to setup signal handlers.");

        goto cleanup;
    }

    log_write(LOG_LEVEL_INFO,
              "Signal handlers initialized.");

    /*
     * 5. 创建 TCP Socket
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        int saved_errno = errno;

        perror("socket");

        log_write(LOG_LEVEL_ERROR,
                  "socket failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    /*
     * 6. 设置监听 Socket 为非阻塞模式
     */
    if (set_nonblocking(server_fd) == -1) {
        log_write(LOG_LEVEL_ERROR,
                  "Failed to set server socket nonblocking.");

        goto cleanup;
    }

    /*
     * 7. 设置端口复用
     */
    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) == -1) {

        int saved_errno = errno;

        perror("setsockopt");

        log_write(LOG_LEVEL_ERROR,
                  "setsockopt failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    /*
     * 8. 初始化服务器地址
     */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /*
     * 9. 绑定端口
     */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {

        int saved_errno = errno;

        perror("bind");

        log_write(LOG_LEVEL_ERROR,
                  "bind failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    /*
     * 10. 开始监听
     */
    if (listen(server_fd, 16) == -1) {
        int saved_errno = errno;

        perror("listen");

        log_write(LOG_LEVEL_ERROR,
                  "listen failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    /*
     * 11. 创建 epoll 实例
     */
    epfd = epoll_create1(0);

    if (epfd == -1) {
        int saved_errno = errno;

        perror("epoll_create1");

        log_write(LOG_LEVEL_ERROR,
                  "epoll_create1 failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    /*
     * 12. 将监听 Socket 注册到 epoll，使用 ET 模式
     */
    memset(&ev, 0, sizeof(ev));

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;

    if (epoll_ctl(epfd,
                  EPOLL_CTL_ADD,
                  server_fd,
                  &ev) == -1) {

        int saved_errno = errno;

        perror("epoll_ctl ADD server_fd");

        log_write(LOG_LEVEL_ERROR,
                  "epoll_ctl ADD server_fd failed: %s",
                  strerror(saved_errno));

        goto cleanup;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);
    printf("Maximum clients: %d\n", MAX_CLIENTS);
    printf("I/O multiplexing: epoll ET (all sockets)\n");
    printf("Nonblocking send with EPOLLOUT enabled.\n");
    printf("Device manager initialized.\n");

    log_write(LOG_LEVEL_INFO,
              "Server started: Port=%d, Max Clients=%d",
              SERVER_PORT,
              MAX_CLIENTS);

    /*
     * 13. epoll 事件循环
     *
     * 收到 SIGINT 或 SIGTERM 后，
     * 信号处理函数会将 g_stop 设置为 1，
     * 主循环检测到后退出。
     */
    while (!g_stop) {

        int n = epoll_wait(
            epfd,
            events,
            MAX_EVENTS,
            1000
        );

        if (n == -1) {

            /*
             * 系统调用被信号中断
             */
            if (errno == EINTR) {

                if (g_stop) {
                    break;
                }

                check_devices(&manager);
                check_server_stats();

                continue;
            }

            int saved_errno = errno;

            perror("epoll_wait");

            log_write(LOG_LEVEL_ERROR,
                      "epoll_wait failed: %s",
                      strerror(saved_errno));

            break;
        }

        /*
         * 即使 epoll_wait 返回了事件，
         * 也要先检查是否收到了退出信号。
         */
        if (g_stop) {
            break;
        }

        /*
         * 定期检查设备心跳超时
         */
        check_devices(&manager);

        /*
         * 定期打印服务器运行统计
         */
        check_server_stats();

        /*
         * 遍历本次返回的就绪事件
         */
        for (int i = 0; i < n; i++) {

            /*
             * 不再处理新的事件
             */
            if (g_stop) {
                break;
            }

            int fd = events[i].data.fd;
            uint32_t event_flags = events[i].events;

            /*
             * 情况一：监听 Socket 有新连接
             */
            if (fd == server_fd) {

                /*
                 * ET 模式下必须循环 accept
                 */
                while (!g_stop) {

                    struct sockaddr_in client_addr;
                    socklen_t addr_len = sizeof(client_addr);

                    int new_fd = accept(
                        server_fd,
                        (struct sockaddr *)&client_addr,
                        &addr_len
                    );

                    if (new_fd == -1) {

                        if (errno == EINTR) {

                            if (g_stop) {
                                break;
                            }

                            continue;
                        }

                        if (errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            break;
                        }

                        int saved_errno = errno;

                        perror("accept");

                        log_write(LOG_LEVEL_ERROR,
                                  "accept failed: %s",
                                  strerror(saved_errno));

                        break;
                    }

                    /*
                     * 如果刚刚 accept 成功就收到退出信号，
                     * 直接关闭新连接，不再加入客户端列表。
                     */
                    if (g_stop) {
                        close(new_fd);
                        break;
                    }

                    /*
                     * 查找空闲客户端槽位
                     */
                    int slot = find_free_slot(client_fds);

                    if (slot == -1) {

                        printf("Maximum clients reached. Rejecting connection.\n");

                        log_write(LOG_LEVEL_WARN,
                                  "Maximum clients reached. Rejecting FD=%d",
                                  new_fd);

                        close(new_fd);
                        continue;
                    }

                    /*
                     * 新客户端 Socket 设置为非阻塞
                     */
                    if (set_nonblocking(new_fd) == -1) {

                        log_write(LOG_LEVEL_ERROR,
                                  "Failed to set client FD=%d nonblocking.",
                                  new_fd);

                        close(new_fd);
                        continue;
                    }

                    /*
                     * 客户端 Socket 使用 ET 模式
                     */
                    struct epoll_event client_ev;

                    memset(&client_ev, 0, sizeof(client_ev));

                    client_ev.events =
                        EPOLLIN | EPOLLRDHUP | EPOLLET;

                    client_ev.data.fd = new_fd;

                    if (epoll_ctl(epfd,
                                  EPOLL_CTL_ADD,
                                  new_fd,
                                  &client_ev) == -1) {

                        int saved_errno = errno;

                        perror("epoll_ctl ADD client");

                        log_write(LOG_LEVEL_ERROR,
                                  "epoll_ctl ADD client failed: FD=%d, Error=%s",
                                  new_fd,
                                  strerror(saved_errno));

                        close(new_fd);
                        continue;
                    }

                    /*
                     * 连接正式加入服务器
                     */
                    client_fds[slot] = new_fd;

                    g_stats.total_connections++;
                    g_stats.active_connections++;

                    protocol_parser_init(&parsers[slot]);

                    memset(&send_buffers[slot],
                           0,
                           sizeof(send_buffers[slot]));

                    char client_ip[INET_ADDRSTRLEN];

                    if (inet_ntop(AF_INET,
                                  &client_addr.sin_addr,
                                  client_ip,
                                  sizeof(client_ip)) == NULL) {

                        strcpy(client_ip, "unknown");
                    }

                    printf("\nClient connected: %s:%d\n",
                           client_ip,
                           ntohs(client_addr.sin_port));

                    printf("Client fd: %d, Slot: %d\n",
                           new_fd,
                           slot);

                    log_write(LOG_LEVEL_INFO,
                              "Client connected: IP=%s, Port=%d, FD=%d, Slot=%d",
                              client_ip,
                              ntohs(client_addr.sin_port),
                              new_fd,
                              slot);
                }

                continue;
            }

            /*
             * 情况二：客户端 Socket 发生事件
             */
            int slot = find_client_slot(client_fds, fd);

            if (slot == -1) {
                continue;
            }

            /*
             * 连接发生错误
             */
            if (event_flags & EPOLLERR) {

                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);

                if (getsockopt(fd,
                               SOL_SOCKET,
                               SO_ERROR,
                               &socket_error,
                               &error_len) == -1) {

                    socket_error = errno;
                }

                printf("Client socket error: fd=%d\n", fd);

                log_write(LOG_LEVEL_ERROR,
                          "Client socket error: FD=%d, Error=%s",
                          fd,
                          strerror(socket_error));

                disconnect_client(
                    epfd,
                    client_fds,
                    parsers,
                    send_buffers,
                    slot
                );

                continue;
            }

            /*
             * 先处理可读事件和对端关闭事件
             */
            if (event_flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {

                if (handle_client_data(
                        epfd,
                        client_fds,
                        parsers,
                        send_buffers,
                        &manager,
                        slot,
                        fd) < 0) {

                    continue;
                }
            }

            /*
             * 处理可写事件
             */
            if (event_flags & EPOLLOUT) {

                if (flush_client_send_buffer(
                        epfd,
                        fd,
                        &send_buffers[slot]) < 0) {

                    disconnect_client(
                        epfd,
                        client_fds,
                        parsers,
                        send_buffers,
                        slot
                    );

                    continue;
                }
            }
        }
    }

    /*
     * 14. 判断退出原因
     */
    if (g_stop) {

        exit_status = EXIT_SUCCESS;

        log_write(LOG_LEVEL_INFO,
                  "Shutdown signal received. Preparing graceful shutdown.");

        printf("\nShutdown signal received. Preparing graceful shutdown.\n");

    } else {

        exit_status = EXIT_FAILURE;

        log_write(LOG_LEVEL_ERROR,
                  "Server event loop exited unexpectedly.");
    }

cleanup:

    /*
     * 15. 打印最终统计信息
     */
    print_server_stats();

    /*
     * 16. 打印最终设备状态
     */
    printf("\n========== Final Device Status ==========\n");

    for (size_t i = 0; i < manager.count; i++) {
        print_device_info(&manager.devices[i]);
    }

    printf("=========================================\n");

    /*
     * 17. 释放所有客户端
     */
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (client_fds[i] >= 0) {

            if (epfd >= 0) {
                epoll_ctl(
                    epfd,
                    EPOLL_CTL_DEL,
                    client_fds[i],
                    NULL
                );
            }

            close(client_fds[i]);

            client_fds[i] = -1;
        }
    }

    /*
     * 18. 释放监听 Socket 和 epoll 实例
     */
    if (server_fd >= 0) {
        close(server_fd);
    }

    if (epfd >= 0) {
        close(epfd);
    }

    /*
     * 19. 记录服务器退出并关闭日志
     */
    log_write(LOG_LEVEL_INFO, "Server stopped.");

    log_close();

    return exit_status;
}
