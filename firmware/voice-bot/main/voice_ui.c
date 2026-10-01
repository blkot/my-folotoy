#include "voice_ui.h"

#include "bsp_display.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// 由 assets/fonts/vn_font_16.c 生成的 GB2312 16px 中文字库。
LV_FONT_DECLARE(vn_font_16);

// 布局常量。面板是 240x320。
#define STATUS_H     22
#define CHAT_TOP     24
#define CHAT_BOTTOM  248
#define LEVEL_TOP    250
#define FOOTER_TOP   266
#define MSG_WIDTH    212
#define MAX_MESSAGES 24

static lv_obj_t *s_dot;
static lv_obj_t *s_state;
static lv_obj_t *s_battery;
static lv_obj_t *s_volume;
static lv_obj_t *s_chat;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_footer;
static lv_obj_t *s_settings;
static lv_obj_t *s_settings_rows[2];
static lv_obj_t *s_settings_values[2];

static int s_message_count;
static int s_settings_selected;

// --------------------------------------------------------------------------
// 颜色
// --------------------------------------------------------------------------
static uint32_t state_color(voice_ui_state_t state) {
    switch (state) {
        case VOICE_UI_LISTENING: return 0xF0A030;   // 橙:在听
        case VOICE_UI_THINKING:  return 0x5B9BF0;   // 蓝:在想
        case VOICE_UI_SPEAKING:  return 0x4FD0C8;   // 青:在说
        case VOICE_UI_OFFLINE:   return 0xE05050;   // 红:断线
        default:                 return 0x66CC88;   // 绿:就绪
    }
}

static uint32_t role_color(voice_ui_role_t role) {
    switch (role) {
        case VOICE_UI_USER:   return 0x2E4A6B;   // 深蓝:你说的话
        case VOICE_UI_BOT:    return 0x1E3A34;   // 深绿:AI 的话
        default:              return 0x3A2E1E;   // 深棕:系统提示
    }
}

static uint32_t role_text_color(voice_ui_role_t role) {
    switch (role) {
        case VOICE_UI_USER:   return 0xBBD4F0;
        case VOICE_UI_BOT:    return 0xA8E6D0;
        default:              return 0xE0C89A;
    }
}

// --------------------------------------------------------------------------
// 状态栏
// --------------------------------------------------------------------------
static lv_obj_t *make_status_label(lv_obj_t *parent, int x, const lv_font_t *font,
                                   uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, x, 0);
    return label;
}

// --------------------------------------------------------------------------
// 聊天区
// --------------------------------------------------------------------------
static void chat_trim(void) {
    // 无触摸时消息只能靠自动滚动,不需要留太多历史;保留少量以便回看。
    while (s_message_count > MAX_MESSAGES) {
        lv_obj_t *oldest = lv_obj_get_child(s_chat, 0);
        if (oldest == NULL) break;
        lv_obj_delete(oldest);
        s_message_count--;
    }
}

static void chat_scroll_to_bottom(void) {
    lv_obj_update_layout(s_chat);
    lv_obj_scroll_to_y(s_chat, LV_COORD_MAX, LV_ANIM_OFF);
}

// --------------------------------------------------------------------------
// 构建
// --------------------------------------------------------------------------
void voice_ui_build(void) {
    lv_obj_t *screen = lv_screen_active();
    // 只设颜色,不动 bg_opa:切换透明度会触发整屏重绘,在直绘场景下有踩坑史。
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // ---- 状态栏 ----
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_set_size(status, 240, STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x161C26), 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    s_dot = lv_obj_create(status);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_align(s_dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x66CC88), 0);

    s_state = make_status_label(status, 22, &lv_font_montserrat_14, 0xE6EAF2);
    lv_label_set_text(s_state, "BOOT");

    // 右侧:电量百分比 + 音量百分比。用等宽感的小字,避免数字跳动时抖动。
    s_volume = make_status_label(status, 152, &lv_font_montserrat_14, 0x6C7688);
    lv_label_set_text(s_volume, "V--");
    s_battery = make_status_label(status, 200, &lv_font_montserrat_14, 0x8A94A6);
    lv_label_set_text(s_battery, "--%");

    // ---- 聊天区 ----
    s_chat = lv_obj_create(screen);
    lv_obj_set_size(s_chat, 240, CHAT_BOTTOM - CHAT_TOP);
    lv_obj_align(s_chat, LV_ALIGN_TOP_LEFT, 0, CHAT_TOP);
    lv_obj_set_style_bg_color(s_chat, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_border_width(s_chat, 0, 0);
    lv_obj_set_style_radius(s_chat, 0, 0);
    lv_obj_set_style_pad_all(s_chat, 4, 0);
    lv_obj_set_style_pad_row(s_chat, 6, 0);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);

    // ---- 录音电平条 ----
    s_level_bar = lv_bar_create(screen);
    lv_obj_set_size(s_level_bar, 212, 6);
    lv_obj_align(s_level_bar, LV_ALIGN_TOP_MID, 0, LEVEL_TOP);
    lv_bar_set_range(s_level_bar, 0, 100);
    lv_bar_set_value(s_level_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_level_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x1E2530), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x3FA9F5), LV_PART_INDICATOR);

    // ---- 底部一行 ----
    s_footer = lv_label_create(screen);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x4A5464), 0);
    lv_label_set_text(s_footer, "");
    lv_obj_align(s_footer, LV_ALIGN_TOP_LEFT, 14, FOOTER_TOP);

    voice_ui_settings_close();
}

// --------------------------------------------------------------------------
// 状态栏更新
// --------------------------------------------------------------------------
void voice_ui_set_state(voice_ui_state_t state, const char *text) {
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(state_color(state)), 0);
    lv_label_set_text(s_state, text);
    lv_obj_set_style_text_color(s_state, lv_color_hex(state_color(state)), 0);
}

void voice_ui_set_footer(const char *text) {
    lv_label_set_text(s_footer, text);
}

void voice_ui_set_level(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_bar_set_value(s_level_bar, percent, LV_ANIM_OFF);
}

void voice_ui_set_battery(int soc) {
    if (soc < 0) {
        lv_label_set_text(s_battery, "--%");
        lv_obj_set_style_text_color(s_battery, lv_color_hex(0x8A94A6), 0);
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
        // 低电量变红,提醒充电。
        const uint32_t color = (soc <= 15) ? 0xE05050 : (soc <= 30 ? 0xE0A030 : 0x8A94A6);
        lv_obj_set_style_text_color(s_battery, lv_color_hex(color), 0);
    }
}

void voice_ui_set_volume(int percent) {
    lv_label_set_text_fmt(s_volume, "V%d", percent);
}

// --------------------------------------------------------------------------
// 聊天
// --------------------------------------------------------------------------
void voice_ui_add_message(voice_ui_role_t role, const char *text) {
    if (text == NULL || text[0] == '\0') return;

    lv_obj_t *bubble = lv_obj_create(s_chat);
    lv_obj_set_width(bubble, MSG_WIDTH);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(role_color(role)), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    // 你说的话靠右,AI 的靠左 —— 一眼能分辨谁在说。
    lv_obj_set_style_margin_left(bubble, role == VOICE_UI_USER ? 12 : 0, 0);
    lv_obj_set_style_margin_right(bubble, role == VOICE_UI_USER ? 0 : 12, 0);

    lv_obj_t *label = lv_label_create(bubble);
    lv_obj_set_style_text_font(label, &vn_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(role_text_color(role)), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, MSG_WIDTH - 16);
    lv_label_set_text(label, text);

    s_message_count++;
    chat_trim();
    chat_scroll_to_bottom();
}

void voice_ui_clear_messages(void) {
    lv_obj_clean(s_chat);
    s_message_count = 0;
}

// --------------------------------------------------------------------------
// 设置页
// --------------------------------------------------------------------------
static lv_obj_t *settings_make_row(int index, const char *name) {
    lv_obj_t *row = lv_obj_create(s_settings);
    lv_obj_set_size(row, 208, 46);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2130), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &vn_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xD8DEE9), 0);
    lv_label_set_text(title, name);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0x3FA9F5), 0);
    lv_label_set_text(value, "--");
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -4, 0);

    s_settings_rows[index] = row;
    s_settings_values[index] = value;
    return row;
}

void voice_ui_settings_open(void) {
    if (s_settings != NULL) return;

    s_settings = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_settings, 240, 320);
    lv_obj_align(s_settings, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_radius(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 12, 0);
    lv_obj_set_style_pad_row(s_settings, 10, 0);
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_settings);
    lv_obj_set_style_text_font(title, &vn_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8A94A6), 0);
    lv_label_set_text(title, "设置");

    settings_make_row(0, "音量");
    settings_make_row(1, "亮度");

    lv_obj_t *hint = lv_label_create(s_settings);
    lv_obj_set_style_text_font(hint, &vn_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6C7688), 0);
    lv_label_set_text(hint, "上下选择  确定调整\n长按确定退出");

    voice_ui_settings_select(s_settings_selected);
}

void voice_ui_settings_close(void) {
    if (s_settings != NULL) {
        lv_obj_delete(s_settings);
        s_settings = NULL;
        s_settings_rows[0] = s_settings_rows[1] = NULL;
        s_settings_values[0] = s_settings_values[1] = NULL;
    }
}

bool voice_ui_settings_is_open(void) {
    return s_settings != NULL;
}

int voice_ui_settings_selected(void) {
    return s_settings_selected;
}

void voice_ui_settings_select(int index) {
    s_settings_selected = (index < 0 || index > 1) ? 0 : index;
    for (int i = 0; i < 2; i++) {
        if (s_settings_rows[i] == NULL) continue;
        const bool active = (i == s_settings_selected);
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? 0x24314A : 0x1A2130), 0);
        lv_obj_set_style_border_width(s_settings_rows[i], active ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_settings_rows[i], lv_color_hex(0x3FA9F5), 0);
    }
}

void voice_ui_settings_refresh(int volume, int brightness) {
    if (s_settings_values[0] != NULL) {
        lv_label_set_text_fmt(s_settings_values[0], "%d%%", volume);
    }
    if (s_settings_values[1] != NULL) {
        lv_label_set_text_fmt(s_settings_values[1], "%d%%", brightness);
    }
}
