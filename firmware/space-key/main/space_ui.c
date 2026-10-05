// main/space_ui.c —— 状态页(设计语言见 docs/usage-monitor-design.md)。
#include "space_ui.h"

#include "lvgl.h"

#include <stdio.h>

LV_FONT_DECLARE(vn_font_16);

// 调色板(与 usage-monitor 同一套)。
#define C_BG     0x0B0E0D
#define C_TRACE  0x1F2A24
#define C_GREY   0x6B7A72
#define C_WHITE  0xF2F5F3
#define C_MINT   0x2EE59D
#define C_BLUE   0x5B9BFF
#define C_AMBER  0xFFB454

#define SCR_W         240
#define MARGIN_X      6
#define FRAME_TOP     17
#define FRAME_BOTTOM  303
#define PAD           4

static lv_obj_t *s_dot;
static lv_obj_t *s_state;
static lv_obj_t *s_count;

static uint32_t state_color(space_key_state_t state) {
    switch (state) {
        case SPACE_KEY_CONNECTED: return C_MINT;
        case SPACE_KEY_SUSPENDED: return C_AMBER;
        default:                  return C_BLUE;
    }
}

static const char *state_text(space_key_state_t state) {
    switch (state) {
        case SPACE_KEY_CONNECTED: return "已连接";
        case SPACE_KEY_SUSPENDED: return "挂起";
        default:                  return "广播中";
    }
}

// 焊点。
static void add_pad(lv_obj_t *parent, int x, int y) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, PAD, PAD);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, lv_color_hex(C_MINT), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollable(p, false);
}

// 1px 走线,右端焊一个薄荷绿方块。
static void add_trace(lv_obj_t *parent, int y) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCR_W - MARGIN_X * 2, 1);
    lv_obj_set_pos(line, MARGIN_X, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_TRACE), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_scrollable(line, false);
    add_pad(parent, SCR_W - MARGIN_X - PAD, y - (PAD / 2));
}

void space_ui_build(void) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // ---- 页眉 ----
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_GREY), 0);
    lv_label_set_text(title, "BLE SPACE KEY");
    lv_obj_set_pos(title, MARGIN_X, 4);

    s_dot = lv_obj_create(screen);
    lv_obj_set_size(s_dot, PAD, PAD);
    lv_obj_set_pos(s_dot, 150, 9);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_radius(s_dot, 0, 0);
    lv_obj_set_style_pad_all(s_dot, 0, 0);
    lv_obj_set_scrollable(s_dot, false);

    s_state = lv_label_create(screen);
    lv_obj_set_style_text_font(s_state, &vn_font_16, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(C_BLUE), 0);
    lv_label_set_text(s_state, "广播中");
    lv_obj_set_pos(s_state, 158, 2);

    add_trace(screen, FRAME_TOP);

    // ---- 中央键帽 ----
    lv_obj_t *cap = lv_obj_create(screen);
    lv_obj_set_size(cap, 132, 56);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_bg_color(cap, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(C_TRACE), 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_radius(cap, 0, 0);
    lv_obj_set_style_pad_all(cap, 0, 0);
    lv_obj_set_scrollable(cap, false);

    lv_obj_t *cap_label = lv_label_create(cap);
    lv_obj_set_style_text_font(cap_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(cap_label, lv_color_hex(C_WHITE), 0);
    lv_label_set_text(cap_label, "SPACE");
    lv_obj_center(cap_label);

    // ---- 提示 ----
    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_style_text_font(hint, &vn_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_GREY), 0);
    lv_label_set_text(hint, "按 OK 发送空格");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 152);

    // ---- 计数 ----
    s_count = lv_label_create(screen);
    lv_obj_set_style_text_font(s_count, &vn_font_16, 0);
    lv_obj_set_style_text_color(s_count, lv_color_hex(C_MINT), 0);
    lv_label_set_text(s_count, "已发送 0 次");
    lv_obj_align(s_count, LV_ALIGN_TOP_MID, 0, 190);

    add_trace(screen, FRAME_BOTTOM);

    // ---- 页脚 ----
    lv_obj_t *footer = lv_label_create(screen);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(C_GREY), 0);
    lv_label_set_text(footer, "my-folotoy");
    lv_obj_set_pos(footer, MARGIN_X, FRAME_BOTTOM + 5);

    lv_obj_t *dev = lv_label_create(screen);
    lv_obj_set_style_text_font(dev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dev, lv_color_hex(C_GREY), 0);
    lv_label_set_text(dev, "Space Key");
    lv_obj_align(dev, LV_ALIGN_TOP_RIGHT, -MARGIN_X, FRAME_BOTTOM + 5);
}

void space_ui_set_state(space_key_state_t state) {
    const uint32_t color = state_color(state);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(color), 0);
    lv_label_set_text(s_state, state_text(state));
}

void space_ui_set_count(int count) {
    lv_label_set_text_fmt(s_count, "已发送 %d 次", count);
}
