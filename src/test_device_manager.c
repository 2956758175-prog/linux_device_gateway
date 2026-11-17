
#include <stdio.h>
#include <time.h>

#include "device_manager.h"

int main(void)
{
    DeviceManager manager;

    printf("========== Device Manager Test ==========\n");

    /* 1. 初始化设备管理器 */
    device_manager_init(&manager);

    printf("\n[Test 1] Initialize Device Manager\n");
    printf("Device count: %zu\n", manager.count);

    /* 2. 注册设备 */
    printf("\n[Test 2] Register Device\n");

    if (device_manager_register(&manager, 1001) == 0) {
        printf("Device 1001 registered successfully.\n");
    } else {
        printf("Device registration failed.\n");
        return -1;
    }

    printf("Device count: %zu\n", manager.count);

    /* 3. 测试重复注册 */
    printf("\n[Test 3] Duplicate Registration\n");

    if (device_manager_register(&manager, 1001) == 0) {
        printf("Duplicate registration handled successfully.\n");
    } else {
        printf("Duplicate registration failed.\n");
        return -1;
    }

    printf("Device count: %zu\n", manager.count);

    /* 4. 测试心跳更新 */
    printf("\n[Test 4] Heartbeat Update\n");

    DeviceInfo *device = &manager.devices[0];

    /* 人为设置一个较早的活动时间 */
    device->last_seen = time(NULL) - 5;

    time_t old_time = device->last_seen;

    if (device_manager_heartbeat(&manager, 1001) == 0) {
        printf("Heartbeat updated successfully.\n");
    } else {
        printf("Heartbeat update failed.\n");
        return -1;
    }

    printf("Old last_seen: %ld\n", (long)old_time);
    printf("New last_seen: %ld\n", (long)device->last_seen);

    /* 5. 测试心跳超时 */
    printf("\n[Test 5] Heartbeat Timeout\n");

    /* 模拟设备已经 20 秒没有活动 */
    device->last_seen = time(NULL) - 20;

    int offline_count =
        device_manager_check_timeout(&manager, 10);

    printf("Offline device count: %d\n", offline_count);
    printf("Device 1001 online status: %d\n", device->online);

    /* 6. 测试设备重新上线 */
    printf("\n[Test 6] Device Reconnect\n");

    if (device_manager_register(&manager, 1001) == 0) {
        printf("Device 1001 reconnected successfully.\n");
    } else {
        printf("Device reconnect failed.\n");
        return -1;
    }

    printf("Device 1001 online status: %d\n", device->online);

    /* 7. 测试设备查询 */
    printf("\n[Test 7] Find Device\n");

    const DeviceInfo *found =
        device_manager_find(&manager, 1001);

    if (found != NULL) {
        printf("Device found.\n");
        printf("Device ID: %u\n", found->device_id);
        printf("Online status: %d\n", found->online);
    } else {
        printf("Device not found.\n");
        return -1;
    }

    printf("\n========== All Tests Completed ==========\n");

    return 0;
}

