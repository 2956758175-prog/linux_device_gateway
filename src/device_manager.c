#include <string.h>
#include <time.h>

#include "device_manager.h"

/*
 * 初始化设备管理器
 */
void device_manager_init(DeviceManager *manager)
{
    if (manager == NULL) {
        return;
    }

    memset(manager, 0, sizeof(DeviceManager));
}

/*
 * 根据设备 ID 查找设备
 */
const DeviceInfo *device_manager_find(const DeviceManager *manager,
                                      uint32_t device_id)
{
    if (manager == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < manager->count; i++) {
        if (manager->devices[i].device_id == device_id) {
            return &manager->devices[i];
        }
    }

    return NULL;
}

/*
 * 注册设备
 *
 * 返回值：
1	新设备注册成功
2	已注册设备重新上线
0	已经在线，重复注册
-1	注册失败
 */

int device_manager_register(DeviceManager *manager,
                            uint32_t device_id)
{
    if (manager == NULL || device_id == 0 ||
        manager->count > MAX_DEVICES) {
        return -1;
    }

    /* 查找设备是否已经注册 */
    for (size_t i = 0; i < manager->count; i++) {

        if (manager->devices[i].device_id == device_id) {

            DeviceInfo *device = &manager->devices[i];

            /* 设备已经在线：重复注册 */
            if (device->online) {
                device->last_seen = time(NULL);
                return 0;
            }

            /* 设备之前离线：重新上线 */
            device->online = 1;
            device->last_seen = time(NULL);

            return 2;
        }
    }

    /* 检查设备数量是否达到上限 */
    if (manager->count >= MAX_DEVICES) {
        return -1;
    }

    /* 创建新的设备记录 */
    DeviceInfo *device = &manager->devices[manager->count];

    device->device_id = device_id;
    device->online = 1;
    device->last_seen = time(NULL);

    manager->count++;

    return 1;
}


/*
 * 更新设备心跳
 *
 * 返回值：
 *   0：更新成功
 *  -1：设备未注册或参数错误
 */
int device_manager_heartbeat(DeviceManager *manager,
                             uint32_t device_id)
{
    if (manager == NULL || device_id == 0) {
        return -1;
    }

    for (size_t i = 0; i < manager->count; i++) {

        if (manager->devices[i].device_id == device_id) {

            manager->devices[i].online = 1;
            manager->devices[i].last_seen = time(NULL);

            return 0;
        }
    }

    return -1;
}

/*
 * 检查设备心跳是否超时
 *
 * 返回值：
 *   超时离线的设备数量
 *  -1：参数错误
 */
int device_manager_check_timeout(DeviceManager *manager,
                                 int timeout_seconds)
{
    if (manager == NULL || timeout_seconds <= 0) {
        return -1;
    }

    int offline_count = 0;
    time_t now = time(NULL);

    for (size_t i = 0; i < manager->count; i++) {

        DeviceInfo *device = &manager->devices[i];

        if (device->online &&
            difftime(now, device->last_seen) >= timeout_seconds) {

            device->online = 0;
            offline_count++;
        }
    }

    return offline_count;
}

