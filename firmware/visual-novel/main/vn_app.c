// main/vn_app.c —— 视觉小说应用:连 Wi-Fi、按需从后端拉取节点、渲染并响应按键。
//
// 并发模型:
//   - 按键回调/输入任务只把事件投入队列(vn_ev_t),不阻塞、不碰 LVGL。
//   - 唯一的 vn 工作任务负责所有 HTTP 拉取与 UI 更新,访问 LVGL 时持锁。
//   - 页面退出遵循仓库约定:先停任务,再由调用方持锁删屏。
#include "vn_app.h"

#include "vn_bg.h"
#include "vn_client.h"
#include "vn_engine.h"
#include "vn_ui.h"
#include "bsp_display.h"
#include "sdkconfig.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "vn";

typedef enum {
    VN_EV_UP,
    VN_EV_DOWN,
    VN_EV_OK,
    VN_EV_EXIT,
    VN_EV_STOP,
} vn_ev_t;

#define VN_WIRE_MAX 1152

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static volatile bool s_net_ready;   // Wi-Fi 已连接并取得 IP
static bool s_wifi_configured;      // 编译期是否配置了 SSID
static volatile bool s_running;
static volatile bool s_exit;

// --------------------------------------------------------------------------
// Wi-Fi (STA)。凭据来自 Kconfig(编译期写入,属配置而非游戏素材)。
// --------------------------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_ready = false;
        if (s_running) {
            ESP_LOGW(TAG, "Wi-Fi 断开,重连中");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_net_ready = true;
        ESP_LOGI(TAG, "已连接 Wi-Fi");
    }
}

static esp_err_t vn_wifi_init(void) {
    if (CONFIG_VN_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "未配置 Wi-Fi(menuconfig: VN_WIFI_SSID/PASSWORD)");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_FAIL;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);

    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, CONFIG_VN_WIFI_SSID, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, CONFIG_VN_WIFI_PASSWORD, sizeof(config.sta.password) - 1);

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) return err;
    return esp_wifi_start();
}

// --------------------------------------------------------------------------
// 节点加载:拉取 + 解析 + 自动跳过 scene/sprite 节点(它们不是用户暂停点);
// 途经的最后一个 bg 与立绘通过出参回传,供调用方判断是否需要重绘场景。
// --------------------------------------------------------------------------
static esp_err_t load_node(const char *start_id, char *id_out, size_t id_cap,
                           vn_node_t *node, char *wire, size_t wire_len,
                           char *scene_bg, size_t bg_cap, bool *bg_seen,
                           char *sprite_out, size_t sprite_cap, bool *sprite_seen) {
    snprintf(id_out, id_cap, "%s", start_id);
    if (bg_seen != NULL) *bg_seen = false;
    if (sprite_seen != NULL) *sprite_seen = false;
    for (int guard = 0; guard < 32; guard++) {
        esp_err_t err = vn_client_fetch_node(id_out, wire, wire_len);
        if (err != ESP_OK) return err;
        if (vn_node_parse(wire, node) != 0) return ESP_ERR_INVALID_RESPONSE;

        if (node->kind == VN_KIND_SCENE) {
            if (scene_bg != NULL && node->bg[0] != '\0') {
                snprintf(scene_bg, bg_cap, "%s", node->bg);
                if (bg_seen != NULL) *bg_seen = true;
            }
        } else if (node->kind == VN_KIND_SPRITE) {
            // 空 sprite 表示隐藏立绘。
            snprintf(sprite_out, sprite_cap, "%s", node->sprite);
            if (sprite_seen != NULL) *sprite_seen = true;
        } else {
            return ESP_OK;
        }
        if (node->next[0] == '\0') return ESP_OK;
        snprintf(id_out, id_cap, "%s", node->next);
    }
    return ESP_ERR_INVALID_RESPONSE;  // scene/sprite 链异常长,视为后端内容错误
}

// 在持有 LVGL 锁的前提下直绘场景(背景 + 立绘),随后只重绘对话框面板。
// 注意:不能改 screen 的 bg_opa——样式变更会让 LVGL 整屏失效,透明区域在绘制
// 缓冲里是未定义内容,会把直绘的场景整片刷掉(表现为白屏)。screen 背景保持
// 不透明深色且之后不再失效,直绘内容就会一直保留。
static bool draw_background(const char *scene_id, const char *sprite_id) {
    if (!bsp_lvgl_lock(2000)) return false;
    const esp_err_t err = vn_bg_draw(scene_id, sprite_id);
    if (err == ESP_OK) {
        vn_ui_repaint();  // 场景覆盖了对话框,重绘面板
    } else {
        ESP_LOGW(TAG, "场景绘制失败: %s", esp_err_to_name(err));
    }
    bsp_lvgl_unlock();
    return err == ESP_OK;
}

static void show_status(const char *text) {
    if (bsp_lvgl_lock(500)) {
        vn_ui_set_status(text);
        bsp_lvgl_unlock();
    }
}

static void show_node(const vn_node_t *node) {
    if (bsp_lvgl_lock(500)) {
        vn_ui_show_node(node);
        bsp_lvgl_unlock();
    }
}

// 等到拿到 IP 再开始拉取,避免请求抢在 DHCP 之前失败。期间可被 STOP/EXIT 打断。
static bool wait_for_network(void) {
    bool announced = false;
    while (s_running) {
        if (s_net_ready) return true;
        if (!announced) {
            show_status("Connecting Wi-Fi...");
            announced = true;
        }
        vn_ev_t ev;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (ev == VN_EV_STOP) return false;
            if (ev == VN_EV_EXIT) {
                s_exit = true;
                return false;
            }
        }
    }
    return false;
}

static void vn_worker(void *arg) {
    (void)arg;
    static char wire[VN_WIRE_MAX];
    char id[VN_ID_MAX];
    char load_from[VN_ID_MAX];
    char next_bg[VN_ID_MAX];
    char next_sprite[VN_ID_MAX];
    char current_bg[VN_ID_MAX] = "";
    char current_sprite[VN_ID_MAX] = "";
    bool bg_seen = false;
    bool sprite_seen = false;
    vn_node_t node;
    bool finished = false;

    snprintf(load_from, sizeof(load_from), "%s", CONFIG_VN_START_NODE);
    s_exit = false;

    while (s_running && !finished) {
        // 1) 先确认 Wi-Fi 已配置并等到取得 IP,再加载节点;失败自动重试。
        if (!s_wifi_configured) {
            show_status("Wi-Fi not configured (Kconfig)");
            vn_ev_t ev;
            if (xQueueReceive(s_queue, &ev, portMAX_DELAY) == pdTRUE && ev == VN_EV_EXIT) {
                s_exit = true;
            }
            finished = true;
            break;
        }
        if (!wait_for_network()) {
            finished = true;
            break;
        }
        for (;;) {
            show_status("Loading...");
            esp_err_t err = load_node(load_from, id, sizeof(id), &node, wire, sizeof(wire),
                                      next_bg, sizeof(next_bg), &bg_seen,
                                      next_sprite, sizeof(next_sprite), &sprite_seen);
            if (err == ESP_OK) break;
            ESP_LOGE(TAG, "拉取节点失败: %s", esp_err_to_name(err));
            show_status("Server unreachable. Retrying...");
            // 每 2 秒自动重试;期间按任意键可立即重试,长按确定退出。
            vn_ev_t retry;
            if (xQueueReceive(s_queue, &retry, pdMS_TO_TICKS(2000)) == pdTRUE) {
                if (retry == VN_EV_STOP) {
                    finished = true;
                    break;
                }
                if (retry == VN_EV_EXIT) {
                    s_exit = true;
                    finished = true;
                    break;
                }
            }
        }
        if (finished) break;
        // 场景或立绘发生变化时,先把场景直绘到面板,再重绘控件。
        const bool bg_changed = bg_seen && strcmp(next_bg, current_bg) != 0;
        const bool sprite_changed = sprite_seen && strcmp(next_sprite, current_sprite) != 0;
        if (bg_changed || sprite_changed) {
            const char *draw_bg = bg_seen ? next_bg : current_bg;
            const char *draw_sprite = sprite_seen ? next_sprite : current_sprite;
            if (draw_bg[0] != '\0' && draw_background(draw_bg, draw_sprite)) {
                if (bg_seen) snprintf(current_bg, sizeof(current_bg), "%s", next_bg);
                if (sprite_seen) {
                    snprintf(current_sprite, sizeof(current_sprite), "%s", next_sprite);
                }
            }
        }
        show_node(&node);

        // 2) 停在当前节点上等输入,直到决定前进或重开。
        const char *target = NULL;
        for (;;) {
            vn_ev_t ev;
            if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
            if (ev == VN_EV_STOP) {
                finished = true;
                break;
            }
            if (ev == VN_EV_EXIT) {
                s_exit = true;
                finished = true;
                break;
            }
            if (ev == VN_EV_UP || ev == VN_EV_DOWN) {
                if (vn_node_is_branching(&node)) {
                    vn_node_select(&node, ev == VN_EV_UP ? -1 : 1);
                    show_node(&node);
                }
                continue;
            }
            // OK:结束节点从头再来,其余按节点规则前进。
            target = (node.kind == VN_KIND_END) ? CONFIG_VN_START_NODE : vn_node_advance(&node);
            break;
        }
        if (finished) break;
        if (target != NULL) {
            snprintf(load_from, sizeof(load_from), "%s", target);
        }
    }

    if (s_done != NULL) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// --------------------------------------------------------------------------
// 生命周期
// --------------------------------------------------------------------------
esp_err_t vn_app_start(void) {
    s_running = true;
    s_exit = false;
    s_net_ready = false;
    s_wifi_configured = CONFIG_VN_WIFI_SSID[0] != '\0';

    if (vn_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 未就绪,仍将启动界面并显示错误");
    }

    if (bsp_lvgl_lock(1000)) {
        vn_ui_build();
        bsp_lvgl_unlock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    s_queue = xQueueCreate(8, sizeof(vn_ev_t));
    s_done = xSemaphoreCreateBinary();
    if (s_queue == NULL || s_done == NULL) {
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
            s_queue = NULL;
        }
        if (s_done != NULL) {
            vSemaphoreDelete(s_done);
            s_done = NULL;
        }
        if (bsp_lvgl_lock(1000)) {
            vn_ui_destroy();
            bsp_lvgl_unlock();
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(vn_worker, "vn", 6144, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_done);
        s_queue = NULL;
        s_done = NULL;
        if (bsp_lvgl_lock(1000)) {
            vn_ui_destroy();
            bsp_lvgl_unlock();
        }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "VN 应用已启动,服务器 %s", vn_client_base_url());
    return ESP_OK;
}

void vn_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_queue == NULL) return;
    vn_ev_t event;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        event = VN_EV_EXIT;
    } else if (ev != BSP_BTN_CLICK) {
        return;
    } else if (btn == BSP_BTN_UP) {
        event = VN_EV_UP;
    } else if (btn == BSP_BTN_DOWN) {
        event = VN_EV_DOWN;
    } else if (btn == BSP_BTN_OK) {
        event = VN_EV_OK;
    } else {
        return;
    }
    (void)xQueueSend(s_queue, &event, 0);
}

bool vn_app_should_exit(void) {
    return s_exit;
}

void vn_app_stop(void) {
    s_running = false;
    if (s_queue != NULL) {
        const vn_ev_t stop = VN_EV_STOP;
        (void)xQueueSend(s_queue, &stop, 0);
    }
    if (s_task != NULL && s_done != NULL) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "VN 工作任务未按期退出");
        }
        s_task = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    if (s_done != NULL) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    if (bsp_lvgl_lock(1000)) {
        vn_ui_destroy();
        bsp_lvgl_unlock();
    }
    s_exit = false;
}
