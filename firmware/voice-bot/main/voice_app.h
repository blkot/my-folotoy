// main/voice_app.h —— 对讲机式语音 bot 的应用层。
//
// 这是派生应用,自带界面(不复用 main/demo_*.c 的测试菜单外壳)。
// 交互:按住「确定」说话 → 松手 → 设备把整段音频流给 PC → PC 回一段音频 → 播放。
#pragma once

#include "esp_err.h"

// 初始化音频与按键 → 建界面 → 启动语音任务。返回后语音任务在后台跑。
esp_err_t voice_app_start(void);
