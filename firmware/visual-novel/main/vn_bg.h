// main/vn_bg.h —— 场景背景:由后端渲染成 RGB565 分条,设备逐条直接刷到 LCD。
#pragma once

#include "esp_err.h"

// 拉取并绘制整幅场景(240x320,背景 + 可选立绘,均由后端合成)。设备不解码
// 图像、不缓存整帧,只用两条 240xN 的缓冲轮流接收。会阻塞若干秒,必须在工作
// 任务中调用,并持有 LVGL 锁以避免与 LVGL 刷屏交叠(直接写面板)。
esp_err_t vn_bg_draw(const char *scene_id, const char *sprite_id);
