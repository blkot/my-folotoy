// main/main.c —— 视觉小说固件入口。
//
// 这是一个派生应用,自带界面(见 vn_ui.c),不复用上游的演示菜单:
// 上游的 main.c 里那 8 项测试菜单(demo_*)与本固件无关,留着只会把
// 上游的代码依赖带进来。
//
// 按键:上/下选择,确定推进剧情,长按确定退出到"空闲"状态。
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "vn_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 按键回调在共享的 esp_timer 任务上跑,所以只入队,绝不阻塞、不碰 LVGL。
typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

#define INPUT_QUEUE_DEPTH 8

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            vn_app_on_key(input.btn, input.event);
        }
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || s_input_queue == NULL) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "vn_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "视觉小说固件启动");

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是这个应用唯一的输出,初始化失败就没法继续。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线"
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 先建按键队列,再让应用开始,避免"应用已跑但按键无人接收"的窗口。
    const esp_err_t input_err = input_dispatch_init();
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    s_input_ready = true;

    const esp_err_t vn_err = vn_app_start();
    if (vn_err != ESP_OK) {
        ESP_LOGE(TAG, "VN 应用启动失败: %s", esp_err_to_name(vn_err));
        return;
    }

    ESP_LOGI(TAG, "就绪");
}
