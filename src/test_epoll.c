
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>

#define MAX_EVENTS 10

int main(void)
{
    int pipe_fd[2];
    int epfd;

    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];

    /* 1. 创建管道 */
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    /* 2. 创建 epoll 实例 */
    epfd = epoll_create1(0);

    if (epfd == -1) {
        perror("epoll_create1");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXIT_FAILURE;
    }

    printf("epoll fd: %d\n", epfd);

    /* 3. 设置监听事件 */
    ev.events = EPOLLIN;
    ev.data.fd = pipe_fd[0];

    /* 4. 将管道读端加入 epoll */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_fd[0], &ev) == -1) {
        perror("epoll_ctl");
        close(epfd);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXIT_FAILURE;
    }

    printf("Pipe read fd registered.\n");

    /* 5. 向管道写入数据 */
    const char *message = "hello epoll";

    if (write(pipe_fd[1], message, 11) == -1) {
        perror("write");
        close(epfd);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXIT_FAILURE;
    }

    printf("Data written to pipe.\n");

    /* 6. 连续等待两次事件 */
    for (int i = 0; i < 2; i++) {

        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);

        if (n == -1) {
            perror("epoll_wait");
            break;
        }

        if (n == 0) {
            printf("Timeout.\n");
            continue;
        }

        for (int j = 0; j < n; j++) {
            printf("Event detected: fd=%d, events=0x%X\n",
                   events[j].data.fd,
                   events[j].events);
        }
    }

    /* 7. 读取管道数据 */
    char buffer[32] = {0};

    ssize_t bytes = read(pipe_fd[0], buffer, sizeof(buffer) - 1);

    if (bytes > 0) {
        printf("Read from pipe: %s\n", buffer);
    }

    /* 8. 释放资源 */
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    close(epfd);

    return EXIT_SUCCESS;
}

