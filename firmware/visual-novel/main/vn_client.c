// main/vn_client.c —— 内容服务器的 HTTP 客户端(阻塞式)。
#include "vn_client.h"

#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "vn_http";

static char s_base_url[96];

const char *vn_client_base_url(void) {
    if (s_base_url[0] == '\0') {
        snprintf(s_base_url, sizeof(s_base_url), "http://%s:%d",
                 CONFIG_VN_SERVER_HOST, CONFIG_VN_SERVER_PORT);
    }
    return s_base_url;
}

// 打开 URL,读取整个响应体;status_out 返回 HTTP 状态码。返回 ESP_OK 表示拿到 2xx。
static esp_err_t vn_get(const char *url, char *buf, size_t buflen, int *status_out) {
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else if (buf != NULL && buflen > 0) {
            int total = 0;
            while (total < (int)buflen - 1) {
                int read = esp_http_client_read(client, buf + total, (int)buflen - 1 - total);
                if (read <= 0) break;
                total += read;
            }
            buf[total] = '\0';
            if (total >= (int)buflen - 1) {
                ESP_LOGW(TAG, "响应被缓冲区截断(%u 字节)", (unsigned)buflen);
            }
        }
    } else {
        ESP_LOGW(TAG, "连接失败: %s (%s)", url, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status_out != NULL) *status_out = status;
    return err;
}

esp_err_t vn_client_fetch_node(const char *node_id, char *buf, size_t buflen) {
    if (node_id == NULL || buf == NULL || buflen == 0) return ESP_ERR_INVALID_ARG;
    char url[160];
    snprintf(url, sizeof(url), "%s/v1/node/%s", vn_client_base_url(), node_id);
    return vn_get(url, buf, buflen, NULL);
}

esp_err_t vn_client_ping(void) {
    char url[128];
    snprintf(url, sizeof(url), "%s/v1/health", vn_client_base_url());
    return vn_get(url, NULL, 0, NULL);
}
