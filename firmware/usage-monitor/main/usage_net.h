// main/usage_net.h —— Wi-Fi 连接与用量数据拉取。
//
// 只做两件事:连 Wi-Fi,然后 GET 一次用量文本。
//
// 为什么用明文 HTTP 而不是 HTTPS:ESP32-C3 可动态分配的主堆只有 41 KB,
// 一次 mbedTLS 握手就要 30-40 KB —— 那会把内存吃光(voice-bot 里 BLE +
// Wi-Fi 共存时只剩 6 KB,连 BLE 都起不来)。用量服务部署在同一个局域网的
// NAS 上,所以走明文是安全与可行性的正确取舍。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

// 连 Wi-Fi(最多约 20 秒)。幂等。
esp_err_t usage_net_wifi_connect(void);

// Wi-Fi 是否已取得 IP。
bool usage_net_wifi_ready(void);

// 拉取用量文本到 buf(自动补 '\0')。阻塞调用。
// 返回 ESP_OK 表示 HTTP 200 且内容已放入 buf。
esp_err_t usage_net_fetch(char *buf, size_t size);
