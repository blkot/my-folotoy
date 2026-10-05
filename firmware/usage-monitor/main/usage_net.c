#include "usage_net.h"

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "u_net";

static bool s_wifi_started;
static volatile bool s_ip_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip_ready = false;
        if (s_wifi_started) {
            ESP_LOGW(TAG, "Wi-Fi 断开,重连中");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_ip_ready = true;
    }
}

bool usage_net_wifi_ready(void) {
    return s_ip_ready;
}

esp_err_t usage_net_wifi_connect(void) {
    if (s_wifi_started) return ESP_OK;

    if (CONFIG_USAGE_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "未配置 Wi-Fi:请用 menuconfig 填 USAGE_WIFI_SSID");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    if (!s_netif_ready) {
        if ((err = esp_netif_init()) != ESP_OK) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_event_loop_ready = true;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_FAIL;

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);

    wifi_config_t config = { 0 };
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s",
             CONFIG_USAGE_WIFI_SSID);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s",
             CONFIG_USAGE_WIFI_PASSWORD);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) return err;

    s_wifi_started = true;
    ESP_LOGI(TAG, "连接 Wi-Fi ...");
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    // ⚠️ esp_wifi_start() 只启动驱动,此刻还没有 IP。必须在这里等到拿到
    // IP 再返回 —— 否则调用方紧接着去拉数据会因"Wi-Fi 未就绪"失败,
    // 而那看起来像"第一次刷新总是失败,等一个周期后才好"(实测 29 秒)。
    for (int waited = 0; waited < 20000 && !s_ip_ready; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ip_ready) {
        ESP_LOGE(TAG, "Wi-Fi 未取得 IP");
        return ESP_ERR_TIMEOUT;
    }

    // 用量轮询是低频小请求,但默认的省电模式会让每次请求都要等一个 beacon
    // 周期(约 102 ms),响应体虽然小、来回几次握手就明显变慢。关掉它。
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

// HTTP 读取回调:把响应体追加到调用方的缓冲,超长就截断。
typedef struct {
    char *buffer;
    size_t size;
    size_t used;
    bool overflowed;
} fetch_context_t;

static esp_err_t on_http_data(esp_http_client_event_t *event) {
    fetch_context_t *context = (fetch_context_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || context == NULL) return ESP_OK;

    // 留一个字节给结尾的 '\0'。
    const size_t room = context->size - 1 - context->used;
    if (room == 0) {
        context->overflowed = true;
        return ESP_OK;
    }

    size_t length = (size_t)event->data_len;
    if (length > room) {
        length = room;
        context->overflowed = true;
    }
    memcpy(context->buffer + context->used, event->data, length);
    context->used += length;
    return ESP_OK;
}

esp_err_t usage_net_fetch(char *buf, size_t size) {
    if (buf == NULL || size < 2) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    if (!s_ip_ready) {
        ESP_LOGW(TAG, "Wi-Fi 未就绪");
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s", CONFIG_USAGE_SERVER_HOST,
             CONFIG_USAGE_SERVER_PORT, CONFIG_USAGE_SERVER_PATH);

    fetch_context_t context = { .buffer = buf, .size = size, .used = 0,
                                .overflowed = false };
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .event_handler = on_http_data,
        .user_data = &context,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    buf[context.used] = '\0';

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "拉取失败: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "服务器返回 HTTP %d", status);
        return ESP_ERR_NOT_FOUND;
    }
    if (context.overflowed) {
        // 截断的内容可能解析到一半,宁可不显示也不要显示错的。
        ESP_LOGW(TAG, "响应超过 %u 字节被截断", (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "拉取到 %u 字节", (unsigned)context.used);
    return ESP_OK;
}
