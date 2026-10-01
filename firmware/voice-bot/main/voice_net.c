#include "voice_net.h"

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "v_net";

#define SOCK_TIMEOUT_S 10

static bool s_wifi_started;
static volatile bool s_ip_ready;
static int s_sock = -1;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 只标记未就绪;关 socket 由使用它的任务负责,避免在这里动别人的 fd。
        s_ip_ready = false;
        if (s_wifi_started) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_ip_ready = true;
    }
}

static esp_err_t wifi_bring_up(void) {
    if (s_wifi_started) return ESP_OK;
    if (CONFIG_VOICE_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "未配置 Wi-Fi:请设置 CONFIG_VOICE_WIFI_SSID / _PASSWORD");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    if ((err = esp_netif_init()) != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_FAIL;

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);

    wifi_config_t config = { 0 };
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", CONFIG_VOICE_WIFI_SSID);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s",
             CONFIG_VOICE_WIFI_PASSWORD);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK) return err;

    s_wifi_started = true;
    ESP_LOGI(TAG, "连接 Wi-Fi \"%s\" ...", CONFIG_VOICE_WIFI_SSID);
    return esp_wifi_start();
}

// 关闭 socket。
//
// ⚠️ 只调 close() 是不够的:lwip 上它只发出第一个 FIN 就返回,而对端(Python
// 的 socketserver)在我们不再说话之前不会关,于是描述符会一直卡在 FIN-WAIT-2,
// 直到超时才回收。重连循环里每 3 秒漏一个 socket,很快就会耗尽
// LWIP_MAX_SOCKETS —— 之后能连上(三次握手由内核完成)却永远收不到应用层回复,
// 表现就是"连上了但永不握手"。
// 先 shutdown(SHUT_RDWR) 主动把两个方向都关掉,对端立即看到 EOF 并释放。
static void close_socket(void) {
    if (s_sock < 0) return;
    shutdown(s_sock, SHUT_RDWR);
    close(s_sock);
    s_sock = -1;
}

static esp_err_t open_socket(void) {
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(CONFIG_VOICE_SERVER_PORT);
    dst.sin_addr.s_addr = inet_addr(CONFIG_VOICE_SERVER_HOST);

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() 失败");
        return ESP_FAIL;
    }

    // 有超时,避免服务器不响应时任务永久挂住。
    const struct timeval timeout = { .tv_sec = SOCK_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    // 语音要的是低延迟而不是高吞吐:关掉 Nagle。
    const int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        ESP_LOGE(TAG, "连接 %s:%d 失败", CONFIG_VOICE_SERVER_HOST, CONFIG_VOICE_SERVER_PORT);
        close(fd);
        return ESP_FAIL;
    }

    s_sock = fd;
    ESP_LOGI(TAG, "已连接 %s:%d", CONFIG_VOICE_SERVER_HOST, CONFIG_VOICE_SERVER_PORT);
    return ESP_OK;
}

static esp_err_t send_all(const void *data, size_t len) {
    const uint8_t *cursor = (const uint8_t *)data;
    while (len > 0) {
        if (s_sock < 0) return ESP_ERR_INVALID_STATE;
        const int sent = send(s_sock, cursor, len, 0);
        if (sent <= 0) return ESP_FAIL;
        cursor += sent;
        len -= (size_t)sent;
    }
    return ESP_OK;
}

static int recv_exact(void *buf, size_t len) {
    uint8_t *cursor = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        if (s_sock < 0) return -1;
        const int received = recv(s_sock, cursor + got, len - got, 0);
        if (received <= 0) return -1;
        got += (size_t)received;
    }
    return (int)got;
}

void voice_net_drop(void) {
    close_socket();
}

bool voice_net_wifi_up(void) {
    return s_ip_ready;
}

bool voice_net_ready(void) {
    return s_ip_ready && s_sock >= 0;
}

esp_err_t voice_net_connect(void) {
    esp_err_t err = wifi_bring_up();
    if (err != ESP_OK) return err;

    for (int waited = 0; waited < 20000 && !s_ip_ready; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ip_ready) {
        ESP_LOGE(TAG, "Wi-Fi 未取得 IP");
        return ESP_ERR_TIMEOUT;
    }

    // 语音链路要低延迟:默认的 WIFI_PS_MIN_MODEM 每 102 ms(一个 beacon)才醒一次。
    esp_wifi_set_ps(WIFI_PS_NONE);

    // ⚠️ 无论 s_sock 是什么,先关掉再连。
    // 之前这里是 `if (s_sock < 0) open()`,后果是:一旦某条失败路径留下了
    // 一个已死但 s_sock >= 0 的句柄,后续所有 connect 都会"复用"它,
    // 每次失败又新建一个 —— 连接在服务端堆积(实测堆到 6 个),设备 socket
    // 很快耗尽,表现为"一直 Connecting"或"说完话没反应"。
    close_socket();

    if ((err = open_socket()) != ESP_OK) return err;

    char hello[64];
    snprintf(hello, sizeof(hello), "HELLO 1 %d 16 1", CONFIG_VOICE_SAMPLE_RATE);
    if ((err = voice_net_send_line(hello)) != ESP_OK) {
        close_socket();
        return err;
    }

    char reply[32];
    if (voice_net_recv_line(reply, sizeof(reply)) < 0) {
        close_socket();
        return ESP_FAIL;
    }
    if (strncmp(reply, "OK", 2) != 0) {
        ESP_LOGE(TAG, "握手被拒绝: %s", reply);
        close_socket();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "语音服务器就绪");
    return ESP_OK;
}

esp_err_t voice_net_reconnect(void) {
    close_socket();
    return voice_net_connect();
}

esp_err_t voice_net_send_line(const char *line) {
    if (line == NULL) return ESP_ERR_INVALID_ARG;
    if (send_all(line, strlen(line)) != ESP_OK) return ESP_FAIL;
    return send_all("\n", 1);
}

esp_err_t voice_net_send_frame(const void *data, size_t len) {
    if (len == 0 || len > 0xFFFF) return ESP_ERR_INVALID_ARG;
    const uint8_t header[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };
    if (send_all(header, sizeof(header)) != ESP_OK) return ESP_FAIL;
    return send_all(data, len);
}

esp_err_t voice_net_send_end(void) {
    const uint8_t end[2] = { 0, 0 };
    return send_all(end, sizeof(end));
}

int voice_net_recv_frame(void *buf, size_t max, size_t *len_out) {
    uint8_t header[2];
    if (recv_exact(header, sizeof(header)) < 0) return -1;

    const size_t len = ((size_t)header[0] << 8) | header[1];
    if (len == 0) return 0;
    if (len > max) {
        // 协议错位。最常见的原因是服务器在音频帧中间插了文本行
        // ("TX" 会被当成 0x5458 = 21592 字节的长度)。
        // 之前这里直接返回错误并断开,导致一次错位就毁掉整轮对话;
        // 现在只报告错位,由上层用"等待下一段的收尾帧"的方式恢复。
        ESP_LOGE(TAG, "帧长 %u 超过缓冲 %u —— 协议错位", (unsigned)len, (unsigned)max);
        return VN_FRAME_PROTOCOL_ERROR;
    }
    if (recv_exact(buf, len) < 0) return -1;

    if (len_out != NULL) *len_out = len;
    return 1;
}

int voice_net_recv_line(char *buf, size_t max) {
    if (buf == NULL || max < 2) return -1;

    size_t length = 0;
    for (;;) {
        uint8_t ch;
        if (recv_exact(&ch, 1) < 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r') {                    // 容忍 CRLF
            if (length + 1 >= max) return -1;
            buf[length++] = (char)ch;
        }
    }
    buf[length] = '\0';
    return (int)length;
}
