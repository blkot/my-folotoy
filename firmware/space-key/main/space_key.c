// main/space_key.c —— BLE HID 键盘实现。
#include "space_key.h"

#include "esp_hid_gap.h"
#include "esp_hidd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "space_key";

// USB HID usage: 空格。
#define HID_KEY_SPACE 0x2C
// 键盘 report map 用 Report ID 1,输入报告 8 字节。
#define HID_REPORT_ID 1
#define HID_REPORT_LEN 8

// 标准键盘 report map(modifiers + reserved + 6 个键位)。
// 取自 ESP-IDF 官方 esp_hid_device 示例。
static const uint8_t keyboard_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array,Abs)
    0xC0,              // End Collection
};

static esp_hidd_dev_t *s_hid_dev;
static space_key_state_cb_t s_state_cb;
static volatile bool s_connected;

static void notify_state(space_key_state_t state) {
    if (s_state_cb) s_state_cb(state);
}

// esp_hid_gap.c 在链路加密完成后会调用它(见 BLE_GAP_EVENT_ENC_CHANGE)。
// 我们不在那里起发送任务 —— 发送由按键任务驱动,所以留空实现满足链接。
void ble_hid_task_start_up(void) {
}

static void hidd_event_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID 启动,开始广播");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "已连接");
        s_connected = true;
        notify_state(SPACE_KEY_CONNECTED);
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "断开,重新广播");
        s_connected = false;
        notify_state(SPACE_KEY_ADVERTISING);
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONTROL_EVENT:
        if (param->control.control) {
            ESP_LOGI(TAG, "主机退出挂起");
            s_connected = true;
            notify_state(SPACE_KEY_CONNECTED);
        } else {
            ESP_LOGI(TAG, "主机挂起");
            notify_state(SPACE_KEY_SUSPENDED);
        }
        break;
    default:
        break;
    }
}

esp_err_t space_key_init(space_key_state_cb_t cb) {
    s_state_cb = cb;

    esp_err_t ret = esp_hid_gap_init(HIDD_BLE_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, "Space Key");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "广播初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    static esp_hid_raw_report_map_t report_maps[] = {
        { .data = keyboard_report_map, .len = sizeof(keyboard_report_map) },
    };
    static esp_hid_device_config_t config = {
        .vendor_id = 0x16C0,
        .product_id = 0x05DF,
        .version = 0x0100,
        .device_name = "Space Key",
        .manufacturer_name = "my-folotoy",
        .serial_number = "0001",
        .report_maps = report_maps,
        .report_maps_len = 1,
    };

    ret = esp_hidd_dev_init(&config, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HID 设备初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

void space_key_send_space(void) {
    if (s_hid_dev == NULL || !s_connected) {
        ESP_LOGW(TAG, "未连接,忽略按键");
        return;
    }

    uint8_t report[HID_REPORT_LEN] = {0};
    report[2] = HID_KEY_SPACE;  // modifiers, reserved, key1..
    esp_hidd_dev_input_set(s_hid_dev, 0, HID_REPORT_ID, report, HID_REPORT_LEN);

    // 抬起:报告清零,再发一次。
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(report, 0, sizeof(report));
    esp_hidd_dev_input_set(s_hid_dev, 0, HID_REPORT_ID, report, HID_REPORT_LEN);

    ESP_LOGI(TAG, "已发送空格");
}

bool space_key_connected(void) {
    return s_connected;
}
