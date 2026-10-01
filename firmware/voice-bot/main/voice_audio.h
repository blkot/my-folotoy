// main/voice_audio.h —— 语音链路的声音输入输出。
//
// 这一层只做一件事:把 BSP 的音频能力固定成"对讲机需要的格式" ——
// 16 kHz / 16 bit / 单声道,收发双向同时可用(ES8311 + I2S 全双工)。
// 上层(voice_app)只看到 read / write 两个阻塞调用,不碰 I2S 与 codec 细节。
#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

// 打开 codec 并按 CONFIG_VOICE_SAMPLE_RATE 配置。幂等。
esp_err_t voice_audio_init(void);

// 采集一块 PCM(阻塞到采满)。返回实际字节数,<0 表示出错。
// 注意:立体声槽位由 esp_codec_dev 做通道映射,这里拿到的就是单声道。
int voice_audio_read(void *buf, size_t bytes);

// 播放一块 PCM(阻塞到写完)。
esp_err_t voice_audio_write(const void *buf, size_t bytes);

void voice_audio_set_volume(uint8_t percent);
