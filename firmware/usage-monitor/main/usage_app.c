#include "usage_app.h"

#include "usage_model.h"
#include "usage_net.h"
#include "usage_ui.h"

#include "bsp_button.h"
#include "bsp_display.h"

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "u_app";

// bsp_pins.h 里三档分压的判定窗口(mV)。>1900 视为松开。
#define BTN_MV_UP_MAX   150
#define BTN_MV_DOWN_MAX 447
#define BTN_MV_OK_MAX   1900

// 按键轮询间隔。20 ms 足够跟手,又不会占满 CPU。
#define KEY_POLL_MS 20
// 按住多久算长按。
#define LONG_PRESS_MS 700

// 响应缓冲。用量文本是几百字节量级,4 KB 留足余量;
// 超过就截断并报错(宁可不显示,也不显示解析到一半的数据)。
#define RESPONSE_MAX 4096

static usage_snapshot_t s_snapshot;
static int s_index;
static bool s_have_data;
static volatile bool s_refresh_requested;

// --------------------------------------------------------------------------
// 界面小包装(操作 LVGL 一律持锁)
// --------------------------------------------------------------------------
static void ui_state(usage_ui_state_t state, const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    usage_ui_set_state(state, text);
    bsp_lvgl_unlock();
}

static void ui_redraw(void) {
    if (!bsp_lvgl_lock(300)) return;
    usage_ui_show(&s_snapshot, s_index);
    bsp_lvgl_unlock();
}

static void ui_message(const char *text) {
    if (!bsp_lvgl_lock(300)) return;
    usage_ui_show_message(text);
    bsp_lvgl_unlock();
}

static void ui_footer(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    usage_ui_set_footer(text);
    bsp_lvgl_unlock();
}

static void ui_updated(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    usage_ui_set_updated(text);
    bsp_lvgl_unlock();
}

// --------------------------------------------------------------------------
// 取数
// --------------------------------------------------------------------------
static char s_response[RESPONSE_MAX];

static bool refresh(void) {
    ui_state(USAGE_UI_LOADING, "刷新中");

    if (!usage_net_wifi_ready() && usage_net_wifi_connect() != ESP_OK) {
        ui_state(USAGE_UI_OFFLINE, "无网络");
        if (!s_have_data) ui_message("连不上 Wi-Fi\n检查 menuconfig 里的配置");
        return false;
    }

    if (usage_net_fetch(s_response, sizeof(s_response)) != ESP_OK) {
        ui_state(USAGE_UI_OFFLINE, "拉取失败");
        if (!s_have_data) ui_message("拉不到用量数据\n检查服务端地址与端口");
        return false;
    }

    usage_snapshot_t parsed;
    if (usage_parse(s_response, &parsed) != 0) {
        ui_state(USAGE_UI_OFFLINE, "数据为空");
        if (!s_have_data) ui_message("服务端返回的数据无法解析");
        return false;
    }

    s_snapshot = parsed;
    s_have_data = true;
    if (s_index >= s_snapshot.platform_count) s_index = 0;

    ui_state(USAGE_UI_IDLE, "正常");
    ui_redraw();

    // 状态栏右侧显示最近更新时间(设备没有 RTC,只能显示"开机以来多久")。
    const int64_t seconds = esp_timer_get_time() / 1000000;
    char stamp[16];
    snprintf(stamp, sizeof(stamp), "%02d:%02d",
             (int)((seconds / 60) % 100), (int)(seconds % 60));
    ui_updated(stamp);

    ESP_LOGI(TAG, "已更新:%d 个平台", s_snapshot.platform_count);
    return true;
}

// --------------------------------------------------------------------------
// 按键:轮询 ADC 分压,拿干净的按下/松开边沿
// (BSP 的事件回调只有 PRESS/CLICK/LONG,没有"松开",做不了长按判定)
// --------------------------------------------------------------------------
static int poll_key(void) {
    const int mv = bsp_button_read_mv();
    if (mv < 0) return -1;
    if (mv < BTN_MV_UP_MAX) return BSP_BTN_UP;
    if (mv < BTN_MV_DOWN_MAX) return BSP_BTN_DOWN;
    if (mv < BTN_MV_OK_MAX) return BSP_BTN_OK;
    return -1;  // 松开
}

static void change_platform(int delta) {
    if (!s_have_data || s_snapshot.platform_count <= 0) return;

    s_index += delta;
    // 循环:到头的下一步回到另一端,比"卡住"好用。
    if (s_index < 0) s_index = s_snapshot.platform_count - 1;
    if (s_index >= s_snapshot.platform_count) s_index = 0;

    ui_redraw();
}

static void toggle_detail(void) {
    if (!s_have_data) return;

    bool detail = false;
    if (bsp_lvgl_lock(200)) {
        detail = !usage_ui_detail_enabled();
        usage_ui_set_detail(detail);
        bsp_lvgl_unlock();
    }
    ui_redraw();
    ui_footer(detail ? "详情  上下切换" : "概览  上下切换");
}

// --------------------------------------------------------------------------
// 主循环
// --------------------------------------------------------------------------
static void usage_task(void *arg) {
    (void)arg;

    const int refresh_seconds = CONFIG_USAGE_REFRESH_SECONDS;
    int64_t next_refresh_at = 0;

    (void)refresh();
    next_refresh_at = esp_timer_get_time() + (int64_t)refresh_seconds * 1000000;

    int last_key = -1;
    int64_t ok_pressed_at = 0;

    for (;;) {
        const int key = poll_key();
        const int64_t now = esp_timer_get_time();

        if (key != last_key) {
            if (key == BSP_BTN_OK && ok_pressed_at == 0) {
                ok_pressed_at = now;
            } else if (key != BSP_BTN_OK && ok_pressed_at != 0) {
                // 确定键松开:长按=刷新,短按=切换详情。
                const bool is_long = (now - ok_pressed_at) > (LONG_PRESS_MS * 1000LL);
                ok_pressed_at = 0;
                if (is_long) {
                    s_refresh_requested = true;
                } else {
                    toggle_detail();
                }
            } else if (key == BSP_BTN_UP) {
                change_platform(-1);
            } else if (key == BSP_BTN_DOWN) {
                change_platform(+1);
            }
            last_key = key;
        }

        if (s_refresh_requested) {
            s_refresh_requested = false;
            (void)refresh();
            next_refresh_at = now + (int64_t)refresh_seconds * 1000000;
        } else if (refresh_seconds > 0 && now >= next_refresh_at) {
            (void)refresh();
            next_refresh_at = now + (int64_t)refresh_seconds * 1000000;
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

esp_err_t usage_app_start(void) {
    // bsp_button_read_mv() 依赖按键驱动建好的 ADC 与校准,所以必须 init;
    // 回调传 NULL:这个应用用轮询,不需要事件。
    const esp_err_t err = bsp_button_init(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键初始化失败(%s),将无法切换平台", esp_err_to_name(err));
    }

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    usage_ui_build();
    usage_ui_set_state(USAGE_UI_LOADING, "启动中");
    usage_ui_show_message("正在获取用量…");
    usage_ui_set_footer("上下切换  确定看详情  长按刷新");
    bsp_lvgl_unlock();

    if (xTaskCreate(usage_task, "usage", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "用量监控就绪:%s:%d%s", CONFIG_USAGE_SERVER_HOST,
             CONFIG_USAGE_SERVER_PORT, CONFIG_USAGE_SERVER_PATH);
    return ESP_OK;
}
