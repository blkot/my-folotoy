// main/voice_prov_radio.c —— 配网所需的 NVS 与 netif 准备。
//
// 逻辑来自上游 demo/blufi-provisioning 分支的 demo_radio.c,去掉了演示框架
// 的前缀。保持简短是有意的:这里只负责"把 Wi-Fi 和 BLE 共同依赖的两个全局
// 服务准备好",不掺业务逻辑。
#include "voice_prov_radio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "v_prov";

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;

esp_err_t voice_prov_radio_nvs_prepare(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // 不自动擦除:NVS 里可能存着用户配好的 Wi-Fi 凭据。
        // 为了能启动无线功能而清掉用户数据是不可接受的。
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t voice_prov_radio_network_prepare(void)
{
    if (!s_netif_ready) {
        const esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        const esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}
