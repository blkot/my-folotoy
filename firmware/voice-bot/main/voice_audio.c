#include "voice_audio.h"

#include "bsp_audio.h"

#include "sdkconfig.h"

#include "esp_log.h"

static const char *TAG = "v_audio";

static bool s_ready;

esp_err_t voice_audio_init(void) {
    if (s_ready) return ESP_OK;

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init 失败: %s —— 用 bsp_i2c_scan() 确认 ES8311 是否应答",
                 esp_err_to_name(err));
        return err;
    }

    // 单声道:硬件是 I2S 立体声槽,通道映射交给 esp_codec_dev。
    // 麦克风 PGA 增益由 BSP 固定为 30 dB(见 bsp_audio.c)。
    err = bsp_audio_set_format(CONFIG_VOICE_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec 打开失败(%d Hz/16bit/1ch): %s", CONFIG_VOICE_SAMPLE_RATE,
                 esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "就绪: %d Hz / 16 bit / 单声道,双向", CONFIG_VOICE_SAMPLE_RATE);
    return ESP_OK;
}

int voice_audio_read(void *buf, size_t bytes) {
    if (!s_ready) return -1;
    // esp_codec_dev_read 会阻塞到读满;成功时正好是 bytes 字节。
    return bsp_audio_read(buf, bytes) == ESP_OK ? (int)bytes : -1;
}

esp_err_t voice_audio_write(const void *buf, size_t bytes) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return bsp_audio_write(buf, bytes);
}

void voice_audio_set_volume(uint8_t percent) {
    bsp_audio_set_volume(percent);
}
