// main/main.c —— AI 语音对讲机(派生应用入口)。
//
// 这是一个派生应用,不是 BSP 演示菜单的复用:界面在 voice_app.c 里自己画。
// main.c 只负责把外设点起来,然后把控制权交给应用层。
//
// 用法:按住「确定」说话,松手后设备把这段音频发给局域网里的 PC,
// PC 处理完回一段音频,设备边收边放。
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "voice_app.h"

#include "esp_log.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "AI 语音对讲机启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是这个应用唯一的反馈渠道,初始化失败就没法继续。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续");
        return;
    }
    bsp_display_backlight(100);

    const esp_err_t err = voice_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "语音应用启动失败: %s", esp_err_to_name(err));
    }
}
