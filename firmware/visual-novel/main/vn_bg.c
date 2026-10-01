// main/vn_bg.c —— 背景分条流:HTTP 拉一条 RGB565,直接 blit 到面板。
#include "vn_bg.h"

#include "vn_client.h"
#include "bsp_display.h"

#include "esp_http_client.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#define BG_WIDTH        240
#define BG_HEIGHT       320
#define BG_STRIP_ROWS   16
#define BG_STRIP_BYTES  (BG_WIDTH * BG_STRIP_ROWS * 2)

static const char *TAG = "vn_bg";

// 双缓冲:上一次 SPI 传输可能仍在进行,下一条先写另一块。
static uint8_t s_strip[2][BG_STRIP_BYTES];
static int s_strip_index;

static esp_err_t fetch_strip(const char *scene_id, const char *sprite_id, int y, int rows) {
    char url[224];
    if (sprite_id != NULL && sprite_id[0] != '\0') {
        snprintf(url, sizeof(url), "%s/v1/scene/%s/strip?y=%d&h=%d&sprite=%s",
                 vn_client_base_url(), scene_id, y, rows, sprite_id);
    } else {
        snprintf(url, sizeof(url), "%s/v1/scene/%s/strip?y=%d&h=%d",
                 vn_client_base_url(), scene_id, y, rows);
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) != 200) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            const int want = BG_WIDTH * rows * 2;
            int got = 0;
            while (got < want) {
                const int read = esp_http_client_read(
                    client, (char *)s_strip[s_strip_index] + got, want - got);
                if (read <= 0) break;
                got += read;
            }
            if (got != want) {
                ESP_LOGW(TAG, "背景条长度不足: %d/%d", got, want);
                err = ESP_ERR_INVALID_SIZE;
            }
        }
    } else {
        ESP_LOGW(TAG, "背景条连接失败 y=%d: %s", y, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t vn_bg_draw(const char *scene_id, const char *sprite_id) {
    if (scene_id == NULL || scene_id[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel == NULL) return ESP_ERR_INVALID_STATE;

    for (int y = 0; y < BG_HEIGHT; y += BG_STRIP_ROWS) {
        const int rows = (y + BG_STRIP_ROWS > BG_HEIGHT) ? (BG_HEIGHT - y) : BG_STRIP_ROWS;
        const esp_err_t err = fetch_strip(scene_id, sprite_id, y, rows);
        if (err != ESP_OK) return err;

        const esp_err_t draw = esp_lcd_panel_draw_bitmap(
            panel, 0, y, BG_WIDTH, y + rows, s_strip[s_strip_index]);
        if (draw != ESP_OK) return draw;

        s_strip_index ^= 1;  // 下一块缓冲,避免覆盖在途传输
    }
    ESP_LOGI(TAG, "场景已绘制: %s%s%s", scene_id,
             (sprite_id != NULL && sprite_id[0] != '\0') ? " + " : "",
             (sprite_id != NULL) ? sprite_id : "");
    return ESP_OK;
}
