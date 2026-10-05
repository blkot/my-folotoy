// main/main.c —— 用量监控固件入口。
//
// 派生应用,自带界面(见 usage_ui.c),不复用上游的演示菜单。
// 它把各平台的 AI 用量汇总显示在屏幕上,数据由一个 HTTP 服务提供
// (部署在局域网里,PC 或 NAS 都行;设备只认一个 URL,所以换位置不用改固件)。
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "usage_app.h"

#include "esp_log.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "用量监控启动");

    bsp_i2c_init();

    // 屏幕是这个应用唯一的输出,初始化失败就没法继续。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续");
        return;
    }
    bsp_display_backlight(100);

    const esp_err_t err = usage_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
    }
}
