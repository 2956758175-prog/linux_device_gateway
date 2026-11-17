
#ifndef __DEVICE_MANAGER_H__
#define __DEVICE_MANAGER_H__

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* 最大管理设备数量 */
#define MAX_DEVICES 32

/* 默认心跳超时时间，单位：秒 */
#define HEARTBEAT_TIMEOUT 60

/* 设备信息 */
typedef struct {
    uint32_t device_id;  /* 设备 ID */
    int online;          /* 在线状态：1 在线，0 离线 */
    time_t last_seen;    /* 最后一次活动时间 */
} DeviceInfo;

/* 设备管理器 */
typedef struct {
    DeviceInfo devices[MAX_DEVICES];
    size_t count;
} DeviceManager;

/* 初始化设备管理器 */
void device_manager_init(DeviceManager *manager);

/* 注册设备 */
int device_manager_register(DeviceManager *manager,
                            uint32_t device_id);

/* 更新设备心跳 */
int device_manager_heartbeat(DeviceManager *manager,
                             uint32_t device_id);

/* 检测设备是否超时 */
int device_manager_check_timeout(DeviceManager *manager,
                                 int timeout_seconds);

/* 根据设备 ID 查找设备 */
const DeviceInfo *device_manager_find(const DeviceManager *manager,
                                      uint32_t device_id);

#endif

