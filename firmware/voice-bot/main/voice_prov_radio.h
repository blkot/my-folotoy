#pragma once

#include "esp_err.h"

// Wi-Fi 与 NimBLE 都依赖 NVS。
//
// 这里只做初始化:失败时【不】擦除 NVS —— 里面可能已经存着用户配好的
// Wi-Fi 凭据,为了启动一个功能而清掉它是不对的。
esp_err_t voice_prov_radio_nvs_prepare(void);

// Wi-Fi 默认 STA netif 依赖 netif 与默认事件循环这两个全局服务。
// 它们按应用生命周期保留,不重复创建。
esp_err_t voice_prov_radio_network_prepare(void);
