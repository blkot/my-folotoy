// main/usage_ui.h —— 用量监控的界面。
//
// 布局(240x320):
//   y=0..22    状态栏: 彩色圆点 + 状态 | 更新时间
//   y=22..282  内容区: 当前平台的各个窗口,每个一条进度条
//   y=282..320 底部提示
//
// 一次只显示一个平台(屏幕小,全挤上去每条只剩几像素)。上/下切换平台。
//
// 两种视图:
//   概览 —— 进度条 + 百分比 + 距重置时间
//   详情 —— 进度条 + 百分比 + 服务端给的明细文本(如 "1.3M / 2M tokens")
// 确定键切换。明细的格式由服务端决定,设备只负责显示,这样换数据源时
// 界面不用改。
#pragma once

#include "usage_model.h"

#include <stdbool.h>

typedef enum {
    USAGE_UI_OFFLINE = 0,   // 红:没网/拉取失败
    USAGE_UI_IDLE,          // 绿:数据正常
    USAGE_UI_LOADING,       // 蓝:正在拉取
} usage_ui_state_t;

// 建界面。必须在持有 LVGL 锁时调用。
void usage_ui_build(void);

void usage_ui_set_state(usage_ui_state_t state, const char *text);
void usage_ui_set_footer(const char *text);
// 状态栏右侧的小字,用于显示最近一次成功更新的时刻。
void usage_ui_set_updated(const char *text);

// 用一份快照重绘内容区。index 是要显示的平台序号(会被夹到合法范围)。
void usage_ui_show(const usage_snapshot_t *snapshot, int index);

// 详情视图开关。返回当前是否处于详情视图。
void usage_ui_set_detail(bool enabled);
bool usage_ui_detail_enabled(void);

// 没有任何数据时显示一行提示(比如"等待数据…")。
void usage_ui_show_message(const char *text);
