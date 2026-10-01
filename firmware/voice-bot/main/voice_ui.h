// main/voice_ui.h —— 语音 bot 的界面。
//
// 布局(240x320):
//   y=0..22   状态栏: 彩色圆点 + 状态文字 | 电量 | 音量
//   y=22..250 聊天区: 用户/AI 对话,自动滚到底(无触摸,不靠按键翻页)
//   y=250..262 录音电平条(只在聆听时有意义)
//   y=262..320 提示 / 服务器地址
//
// 设计要点:状态只占一条细栏,把 2/3 的屏幕留给对话内容 —— 之前状态文字
// 占满上半屏,聊天只能挤在 96px 里。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

// 对话角色:决定气泡颜色与对齐方向。
typedef enum {
    VOICE_UI_USER = 0,   // 你说的
    VOICE_UI_BOT,        // AI 回复的
    VOICE_UI_SYSTEM,     // 本地提示(连接失败等)
} voice_ui_role_t;

// 状态栏圆点的颜色语义。
typedef enum {
    VOICE_UI_IDLE = 0,
    VOICE_UI_LISTENING,
    VOICE_UI_THINKING,
    VOICE_UI_SPEAKING,
    VOICE_UI_OFFLINE,
} voice_ui_state_t;

// 建界面。必须在持有 LVGL 锁时调用。
void voice_ui_build(void);

// 状态栏文字 + 圆点颜色。
void voice_ui_set_state(voice_ui_state_t state, const char *text);

// 底部一行小字(提示或服务器地址)。
void voice_ui_set_footer(const char *text);

// 追加一条对话。自动滚到底,并丢弃过旧的消息以免占内存。
void voice_ui_add_message(voice_ui_role_t role, const char *text);

// 清空对话(切换服务器/重连时用)。
void voice_ui_clear_messages(void);

// 录音电平(0-100)。
void voice_ui_set_level(int percent);

// 状态栏右侧的电量与音量。soc < 0 表示未知。
void voice_ui_set_battery(int soc);
void voice_ui_set_volume(int percent);

// 设置页:音量与亮度的可调项。上下键选择,确定键调整。
void voice_ui_settings_open(void);
void voice_ui_settings_close(void);
bool voice_ui_settings_is_open(void);

// 设置页里当前选中的行(0=音量, 1=亮度),供按键处理查询。
int voice_ui_settings_selected(void);
void voice_ui_settings_select(int index);
void voice_ui_settings_refresh(int volume, int brightness);
