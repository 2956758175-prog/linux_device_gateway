#ifndef __LOGGER_H__
#define __LOGGER_H__

#include <stdio.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

/* 初始化日志模块 */
int log_init(const char *filename);

/* 设置日志级别 */
void log_set_level(LogLevel level);

/* 写入日志 */
void log_write(LogLevel level, const char *format, ...);

/* 关闭日志模块 */
void log_close(void);

#endif
