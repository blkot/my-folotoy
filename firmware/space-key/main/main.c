// main/main.c —— BLE 空格键固件入口。
//
// 设备作为 BLE HID 键盘与 PC 配对;配对后按 OK 键,PC 上就敲一下空格。
// 派生应用,自带界面(space_ui.c),不复用上游的演示菜单。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "space_key.h"
#include "space_ui.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

// NimBLE 的配置存储初始化。IDF 示例里也是手动 extern 声明(不在头文件里)。
void ble_store_config_init(void);

static const char *TAG = "main";

static QueueHandle_t s_key_q;
static int s_count;

// 按键回调:运行在 button 组件的共享 esp_timer 任务里,不能阻塞,
// 所以只把事件丢进队列,真正的发送放到 key_task。
static void button_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_PRESS) {
        const uint8_t one = 1;
        xQueueSend(s_key_q, &one, 0);
    }
}

// 连接状态变化:刷新页眉状态点。
static void state_cb(space_key_state_t state) {
    if (bsp_lvgl_lock(200)) {
        space_ui_set_state(state);
        bsp_lvgl_unlock();
    }
}

static void key_task(void *arg) {
    (void)arg;
    uint8_t item;
    for (;;) {
        if (xQueueReceive(s_key_q, &item, portMAX_DELAY) == pdTRUE) {
            space_key_send_space();
            s_count++;
            if (bsp_lvgl_lock(200)) {
                space_ui_set_count(s_count);
                bsp_lvgl_unlock();
            }
        }
    }
}

// NimBLE host 任务。
static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续");
        return;
    }
    bsp_display_backlight(100);

    s_key_q = xQueueCreate(4, sizeof(uint8_t));

    if (bsp_lvgl_lock(1000)) {
        space_ui_build();
        space_ui_set_state(SPACE_KEY_ADVERTISING);
        space_ui_set_count(0);
        bsp_lvgl_unlock();
    }

    if (bsp_button_init(button_cb, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "按键初始化失败,将无法发送空格");
    }

    if (space_key_init(state_cb) != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID 初始化失败");
        return;
    }

    // 启动 NimBLE host。
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ret = esp_nimble_enable(nimble_host_task);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 启动失败: %s", esp_err_to_name(ret));
        return;
    }

    if (xTaskCreate(key_task, "key", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return;
    }

    ESP_LOGI(TAG, "就绪:与 PC 蓝牙配对后,按 OK 发送空格");
}
