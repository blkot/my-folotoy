// main/space_key.h —— BLE HID 键盘:一个"空格键"。
//
// 设备作为 BLE HID 键盘(Keyboard)与 PC 配对。配对后,按设备的 OK 键,
// 就往 PC 发一次空格(按下 + 抬起)。
//
// 实现基于 ESP-IDF 的 esp_hid 组件的 HID device API + NimBLE;
// GAP/广播的胶水代码在 esp_hid_gap.c(取自 IDF 官方 esp_hid_device 示例)。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef enum {
    SPACE_KEY_ADVERTISING = 0,  // 正在广播,等待配对
    SPACE_KEY_CONNECTED,        // 已连接,可以发键
    SPACE_KEY_SUSPENDED,        // 已连接但主机进入挂起
} space_key_state_t;

typedef void (*space_key_state_cb_t)(space_key_state_t state);

// 初始化 BLE HID 键盘并开始广播。cb 可为 NULL,用于界面刷新状态。
esp_err_t space_key_init(space_key_state_cb_t cb);

// 往主机发一次空格(按下 + 抬起)。未连接时是空操作。
void space_key_send_space(void);

// 当前是否已连接(可发键)。
bool space_key_connected(void);
