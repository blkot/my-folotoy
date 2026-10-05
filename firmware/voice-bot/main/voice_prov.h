// main/voice_prov.h —— Wi-Fi 配网(BLUFI over BLE)。
//
// 为什么需要它:固件不能内置 Wi-Fi 凭据 —— 凭据进仓库就是泄漏,写进 CI 构建
// 的固件又没法分享。所以首次启动时让用户用手机 App 通过蓝牙把凭据发进来,
// 设备存进 NVS,之后每次开机自动重连(esp_wifi_set_config 会自己持久化)。
//
// 用的是 Espressif 的 BLUFI 协议,NimBLE 自带支持
// (CONFIG_BT_NIMBLE_BLUFI_ENABLE),配合官方 App 使用:设备广播名为
// BLUFI_FoloPassport,App 里选它、填 Wi-Fi 名称与密码即可。
//
// 凭据存在 NVS 里,由 Wi-Fi 驱动管理;本模块不碰明文。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

// 设备是否已有可用的 Wi-Fi 凭据(即曾经配过网)。
// 用于决定开机是直接连网还是先进配网模式。
bool voice_prov_has_credentials(void);

// 进入配网模式并等待配网完成(阻塞到拿到 IP 或超时)。
//
// ui 回调由调用方提供,用来把状态显示到屏幕上;传 NULL 则只打日志。
// 返回 ESP_OK 表示已配好并拿到 IP。
typedef void (*voice_prov_status_fn)(const char *text, void *user);

esp_err_t voice_prov_run(voice_prov_status_fn on_status, void *user, int timeout_ms);

// 清掉已保存的凭据(下次开机重新配网)。
esp_err_t voice_prov_forget(void);
