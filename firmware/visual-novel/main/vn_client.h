// main/vn_client.h —— 从内容服务器拉取节点(阻塞式,必须在工作任务中调用)。
#pragma once

#include "esp_err.h"
#include <stddef.h>

// GET <server>/v1/node/<node_id>,把紧凑行协议写入 buf(NUL 结尾)。
// 网络错误、非 200 或缓冲区不足都返回非 ESP_OK。调用会阻塞,勿在按键回调/LVGL 任务中调用。
esp_err_t vn_client_fetch_node(const char *node_id, char *buf, size_t buflen);

// GET <server>/v1/health,200 即视为可用。
esp_err_t vn_client_ping(void);

// "http://host:port",来自 Kconfig。
const char *vn_client_base_url(void);
