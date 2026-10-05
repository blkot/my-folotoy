// main/usage_app.h —— 用量监控应用。
//
// 交互(三个键,ADC 分压,不能同时按):
//   上 / 下   切换平台
//   确定      切换 概览 / 详情
//   长按确定   立即刷新
#pragma once

#include "esp_err.h"

// 初始化并启动后台任务。返回后任务在跑。
esp_err_t usage_app_start(void);
