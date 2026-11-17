
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"

/* 单个日志文件最大容量：1 MB */
#define LOG_MAX_SIZE (1024 * 1024)

/* 最多保留的历史日志数量 */
#define LOG_BACKUP_COUNT 5

/* 日志路径最大长度 */
#define LOG_PATH_SIZE 4096

/* 日志文件指针 */
static FILE *log_fp = NULL;

/* 当前允许输出的最低日志级别 */
static LogLevel current_level = LOG_LEVEL_DEBUG;

/* 当前日志文件路径 */
static char log_filename[LOG_PATH_SIZE];

/* 日志级别名称 */
static const char *level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

/*
 * 生成历史日志文件名
 *
 * 例如：
 * server.log + 1 -> server.log.1
 * server.log + 2 -> server.log.2
 */
static int get_backup_filename(char *buf,
                               size_t size,
                               int index)
{
    int n;

    n = snprintf(buf,
                 size,
                 "%s.%d",
                 log_filename,
                 index);

    if (n < 0 || (size_t)n >= size) {
        return -1;
    }

    return 0;
}

/*
 * 日志轮转
 *
 * 返回值：
 *  0：不需要轮转或轮转成功
 * -1：轮转失败
 */
static int rotate_log_file(void)
{
    struct stat st;
    char old_name[LOG_PATH_SIZE];
    char new_name[LOG_PATH_SIZE];

    if (log_fp == NULL) {
        return -1;
    }

    /* 刷新缓冲区，确保文件大小准确 */
    if (fflush(log_fp) == EOF) {
        perror("fflush log file");
        return -1;
    }

    /* 获取当前日志文件信息 */
    if (fstat(fileno(log_fp), &st) == -1) {
        perror("fstat log file");
        return -1;
    }

    /* 未达到最大容量，不需要轮转 */
    if (st.st_size < LOG_MAX_SIZE) {
        return 0;
    }

    /*
     * 关闭当前日志文件。
     * 之后通过文件路径进行重命名。
     */
    if (fclose(log_fp) == EOF) {
        log_fp = NULL;
        perror("fclose log file");
        return -1;
    }

    log_fp = NULL;

    /*
     * 从最旧的备份开始向后移动。
     *
     * .4 -> .5
     * .3 -> .4
     * .2 -> .3
     * .1 -> .2
     *
     * rename() 在目标文件存在时会替换目标文件。
     */
    for (int i = LOG_BACKUP_COUNT - 1; i >= 1; i--) {

        if (get_backup_filename(old_name,
                                sizeof(old_name),
                                i) == -1) {
            perror("get old backup filename");
            goto reopen_current;
        }

        if (get_backup_filename(new_name,
                                sizeof(new_name),
                                i + 1) == -1) {
            perror("get new backup filename");
            goto reopen_current;
        }

        if (rename(old_name, new_name) == -1) {

            /* 源文件不存在，说明该备份还没有产生 */
            if (errno == ENOENT) {
                continue;
            }

            perror("rename backup log");
            goto reopen_current;
        }
    }

    /* 当前日志改名为 server.log.1 */
    if (get_backup_filename(new_name,
                            sizeof(new_name),
                            1) == -1) {
        perror("get first backup filename");
        goto reopen_current;
    }

    if (rename(log_filename, new_name) == -1) {
        perror("rename current log");
        goto reopen_current;
    }

    /* 创建新的当前日志文件 */
    log_fp = fopen(log_filename, "a");

    if (log_fp == NULL) {
        perror("create new log file");
        return -1;
    }

    return 0;

reopen_current:

    /*
     * 轮转失败时，尽可能重新打开当前日志文件，
     * 避免日志模块完全失去写入能力。
     */
    log_fp = fopen(log_filename, "a");

    if (log_fp == NULL) {
        perror("reopen current log");
    }

    return -1;
}

/*
 * 初始化日志模块
 */
int log_init(const char *filename)
{
    int n;

    if (filename == NULL || filename[0] == '\0') {
        return -1;
    }

    /* 防止重复初始化时遗留旧文件流 */
    if (log_fp != NULL) {
        log_close();
    }

    /* 保存当前日志文件路径 */
    n = snprintf(log_filename,
                 sizeof(log_filename),
                 "%s",
                 filename);

    if (n < 0 || (size_t)n >= sizeof(log_filename)) {
        return -1;
    }

    /* 追加模式打开日志文件 */
    log_fp = fopen(log_filename, "a");

    if (log_fp == NULL) {
        perror("fopen log file");
        log_filename[0] = '\0';
        return -1;
    }

    return 0;
}

/*
 * 设置日志级别
 */
void log_set_level(LogLevel level)
{
    if (level >= LOG_LEVEL_DEBUG &&
        level <= LOG_LEVEL_ERROR) {

        current_level = level;
    }
}

/*
 * 写入日志
 */
void log_write(LogLevel level, const char *format, ...)
{
    if (format == NULL || log_fp == NULL) {
        return;
    }

    if (level < current_level ||
        level > LOG_LEVEL_ERROR) {
        return;
    }

    /* 写入日志前检查是否需要轮转 */
    rotate_log_file();

    if (log_fp == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_info;

    if (localtime_r(&now, &tm_info) == NULL) {
        return;
    }

    char time_buf[32];

    if (strftime(time_buf,
                 sizeof(time_buf),
                 "%Y-%m-%d %H:%M:%S",
                 &tm_info) == 0) {
        return;
    }

    va_list args;

    va_start(args, format);

    fprintf(log_fp,
            "[%s] [%s] ",
            time_buf,
            level_names[level]);

    vfprintf(log_fp, format, args);

    fprintf(log_fp, "\n");

    va_end(args);

    /* 及时刷新，减少异常退出时的日志丢失 */
    fflush(log_fp);
}

/*
 * 关闭日志模块
 */
void log_close(void)
{
    if (log_fp != NULL) {
        fclose(log_fp);
        log_fp = NULL;
    }

    log_filename[0] = '\0';
}
