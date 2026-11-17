#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

#include "protocol_parser.h"
#include "device_manager.h"

#define SERVER_PORT 8080
#define RECV_SIZE 512
#define MAX_CLIENTS 16

/* 打印设备信息 */
void print_device_info(const DeviceInfo *device)
{
    if (device == NULL) {
        return;
    }

    printf("Device ID: %u\n", device->device_id);
    printf("Online: %s\n", device->online ? "YES" : "NO");
    printf("Last Seen: %ld\n", (long)device->last_seen);
}

/* 确保完整发送指定长度的数据 */
int send_all(int fd, const unsigned char *data, size_t len)
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
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/* 发送服务端响应帧 */
int send_response(int client_fd, uint32_t device_id, uint8_t status)
{
    unsigned char frame[PROTOCOL_HEADER_LEN + 1 + CRC16_LEN];
    ProtocolHeader header;

    header.header = FRAME_HEADER;
    header.version = PROTOCOL_VERSION;
    header.command = CMD_RESPONSE;
    header.device_id = device_id;
    header.data_len = 1;

    if (encode_header(frame, &header) < 0) {
        fprintf(stderr, "Failed to encode response header.\n");
        return -1;
    }

    frame[PROTOCOL_HEADER_LEN] = status;

    uint16_t crc = crc16_ccitt(
        frame,
        PROTOCOL_HEADER_LEN + 1
    );

    frame[PROTOCOL_HEADER_LEN + 1] =
        (unsigned char)(crc >> 8);

    frame[PROTOCOL_HEADER_LEN + 2] =
        (unsigned char)(crc & 0xFF);

    if (send_all(client_fd, frame, sizeof(frame)) < 0) {
        fprintf(stderr, "Failed to send response.\n");
        return -1;
    }

    printf("Response sent: Device ID=%u, Status=0x%02X\n",
           device_id, status);

    return 0;
}

/* 处理设备业务命令 */
int process_frame(DeviceManager *manager,
                  const ProtocolFrame *frame,
                  int client_fd)
{
    uint8_t status = STATUS_FAILED;

    printf("\n========== Process Command ==========\n");
    printf("Device ID: %u\n", frame->header.device_id);
    printf("Command: 0x%02X\n", frame->header.command);

    switch (frame->header.command) {

    case CMD_REGISTER:
    {
        printf("Command Type: DEVICE REGISTER\n");

        int ret = device_manager_register(
            manager,
            frame->header.device_id
        );

        if (ret == 1) {
            printf("New device registered successfully.\n");
            status = STATUS_SUCCESS;
        } else if (ret == 2) {
            printf("Device reconnected successfully.\n");
            status = STATUS_SUCCESS;
        } else if (ret == 0) {
            printf("Device is already online.\n");
            status = STATUS_ALREADY_ONLINE;
        } else {
            printf("Device registration failed.\n");
            status = STATUS_FAILED;
        }

        break;
    }

    case CMD_HEARTBEAT:
    {
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
            status = STATUS_NOT_REGISTERED;
        }

        break;
    }

    case CMD_DATA:
    {
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
            status = STATUS_NOT_REGISTERED;
        }

        break;
    }

    default:
        printf("Unknown command.\n");
        status = STATUS_FAILED;
        break;
    }

    printf("=====================================\n");

    return send_response(
        client_fd,
        frame->header.device_id,
        status
    );
}

/* 检查设备心跳超时 */
void check_devices(DeviceManager *manager)
{
    int offline_count =
        device_manager_check_timeout(manager, HEARTBEAT_TIMEOUT);

    if (offline_count > 0) {

        printf("\n========== Device Timeout ==========\n");
        printf("%d device(s) went offline.\n", offline_count);

        for (size_t i = 0; i < manager->count; i++) {
            const DeviceInfo *device = &manager->devices[i];

            if (!device->online) {
                print_device_info(device);
            }
        }

        printf("====================================\n");
    }
}

/*
 * 断开指定客户端
 *
 * index：客户端数组下标
 * 只清理对应客户端，不影响其他连接
 */
void disconnect_client(int client_fds[],
                       ProtocolParser parsers[],
                       int index)
{
    if (index < 0 || index >= MAX_CLIENTS) {
        return;
    }

    if (client_fds[index] < 0) {
        return;
    }

    printf("\nClient disconnected: fd=%d\n",
           client_fds[index]);

    if (parsers[index].len > 0) {
        printf("Warning: incomplete frame discarded (%zu bytes).\n",
               parsers[index].len);
    }

    close(client_fds[index]);
    client_fds[index] = -1;

    protocol_parser_init(&parsers[index]);

    printf("Client slot %d released.\n", index);
}

/* 查找空闲客户端槽位 */
int find_free_slot(const int client_fds[])
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] == -1) {
            return i;
        }
    }

    return -1;
}

int main(void)
{
    int server_fd;
    int opt = 1;

    int client_fds[MAX_CLIENTS];

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    unsigned char recv_buffer[RECV_SIZE];

    ProtocolParser parsers[MAX_CLIENTS];
    ProtocolFrame frame;

    DeviceManager manager;

    /* 1. 初始化客户端数组和协议解析器 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
        protocol_parser_init(&parsers[i]);
    }

    /* 2. 初始化设备管理器，只初始化一次 */
    device_manager_init(&manager);

    /* 3. 创建 TCP Socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* 检查监听 fd 是否能够放入 select 的 fd_set */
    if (server_fd >= FD_SETSIZE) {
        fprintf(stderr, "Server fd exceeds FD_SETSIZE.\n");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* 允许端口复用 */
    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) == -1) {

        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* 4. 初始化服务端地址 */
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* 5. 绑定端口 */
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {

        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* 6. 开始监听 */
    if (listen(server_fd, 16) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d...\n", SERVER_PORT);
    printf("Maximum clients: %d\n", MAX_CLIENTS);
    printf("Device manager initialized.\n");

    /*
     * 7. 多客户端事件循环
     */
    while (1) {

        fd_set read_fds;
        struct timeval timeout;

        int max_fd = server_fd;

        FD_ZERO(&read_fds);

        /* 始终监听服务端监听 Socket */
        FD_SET(server_fd, &read_fds);

        /* 将所有已连接客户端加入监听集合 */
        for (int i = 0; i < MAX_CLIENTS; i++) {

            if (client_fds[i] >= 0) {

                FD_SET(client_fds[i], &read_fds);

                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }

        /* 每隔约 1 秒检查一次设备心跳 */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(max_fd + 1,
                         &read_fds,
                         NULL,
                         NULL,
                         &timeout);

        if (ret == -1) {

            if (errno == EINTR) {
                check_devices(&manager);
                continue;
            }

            perror("select");
            break;
        }

        /* 检查设备心跳超时 */
        check_devices(&manager);

        /* select 超时 */
        if (ret == 0) {
            continue;
        }

        /*
         * 情况一：监听 Socket 就绪
         * 有新的客户端连接请求
         */
        if (FD_ISSET(server_fd, &read_fds)) {

            socklen_t addr_len = sizeof(client_addr);

            int new_fd = accept(
                server_fd,
                (struct sockaddr *)&client_addr,
                &addr_len
            );

            if (new_fd == -1) {

                if (errno == EINTR) {
                    continue;
                }

                perror("accept");
            } else {

                /* select 不能监听超出 FD_SETSIZE 的 fd */
                if (new_fd >= FD_SETSIZE) {

                    fprintf(stderr,
                            "New client fd exceeds FD_SETSIZE, rejected.\n");

                    close(new_fd);

                } else {

                    int slot = find_free_slot(client_fds);

                    if (slot == -1) {

                        printf("Maximum clients reached. Rejecting connection.\n");
                        close(new_fd);

                    } else {

                        client_fds[slot] = new_fd;

                        protocol_parser_init(&parsers[slot]);

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
                    }
                }
            }
        }

        /*
         * 情况二：检查所有客户端是否有数据可读
         */
        for (int i = 0; i < MAX_CLIENTS; i++) {

            int client_fd = client_fds[i];

            if (client_fd < 0) {
                continue;
            }

            if (!FD_ISSET(client_fd, &read_fds)) {
                continue;
            }

            ssize_t n = recv(client_fd,
                             recv_buffer,
                             sizeof(recv_buffer),
                             0);

            /* 接收到数据 */
            if (n > 0) {

                printf("\nReceived %zd bytes from client fd=%d, slot=%d.\n",
                       n, client_fd, i);

                /* 追加 TCP 字节流 */
                if (protocol_parser_append(&parsers[i],
                                           recv_buffer,
                                           (size_t)n) < 0) {

                    printf("Protocol parser buffer overflow!\n");

                    disconnect_client(client_fds, parsers, i);
                    continue;
                }

                /* 循环解析当前客户端的完整协议帧 */
                while (1) {

                    int parse_ret =
                        protocol_parser_next(&parsers[i], &frame);

                    if (parse_ret == 1) {

                        printf("\n========== Protocol Frame ==========\n");

                        printf("Device ID: %u\n",
                               frame.header.device_id);

                        printf("Command: 0x%02X\n",
                               frame.header.command);

                        printf("Data Length: %u\n",
                               frame.header.data_len);

                        printf("Payload: ");

                        fwrite(frame.payload,
                               1,
                               frame.header.data_len,
                               stdout);

                        printf("\n====================================\n");

                        /* 处理业务并向对应客户端发送响应 */
                        if (process_frame(&manager,
                                          &frame,
                                          client_fd) < 0) {

                            disconnect_client(client_fds, parsers, i);
                            break;
                        }

                    } else if (parse_ret == 0) {

                        /* 当前数据不足以组成完整帧 */
                        break;

                    } else {

                        printf("Protocol parsing error!\n");

                        disconnect_client(client_fds, parsers, i);
                        break;
                    }
                }

            } else if (n == 0) {

                /* 客户端正常断开 */
                disconnect_client(client_fds, parsers, i);

            } else {

                if (errno == EINTR) {
                    continue;
                }

                perror("recv");

                disconnect_client(client_fds, parsers, i);
            }
        }
    }

    /* 8. 服务端退出时打印设备状态 */
    printf("\n========== Final Device Status ==========\n");

    for (size_t i = 0; i < manager.count; i++) {
        print_device_info(&manager.devices[i]);
    }

    printf("=========================================\n");

    /* 9. 释放所有客户端资源 */
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (client_fds[i] >= 0) {
            close(client_fds[i]);
            client_fds[i] = -1;
        }
    }

    /* 10. 释放监听 Socket */
    close(server_fd);

    return EXIT_SUCCESS;
}