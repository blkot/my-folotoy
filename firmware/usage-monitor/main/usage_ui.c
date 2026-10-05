// main/usage_ui.c —— 用量监控的界面。
//
// 设计语言来自社区固件「FoloToy AI Passport 本护照」(ai-passport-5),完整规范见
// docs/usage-monitor-design.md。三条要点:
//   1. 电路板丝印:1px 走线做分隔,右端焊一个薄荷绿"焊点"。
//   2. 主色即状态:薄荷绿只给"正在发生"(焊点、正常用量);琥珀/红只给用量档位。
//   3. 只有 3 档字号:大字平台名 / 中字百分比 / 小字大写标签;层级靠留白不靠颜色。
//
// 布局(240x320,边距 6px):
//   y=4..16   页眉: USAGE MONITOR + 状态点 + 更新时间
//   y=17      外框顶线 (焊点)
//   y=24..52  平台名(大字) + 页码
//   y=52      走线 (焊点)
//   y=60..    每个窗口一块:标签 + 百分比 / 进度条 / 重置时间或明细
//   y=303     外框底线 (焊点)
//   y=307..   页脚: 操作提示 + 页码
//
// 两种视图:概览看"多久后重置",详情看服务端给的明细文本。确定键切换。
#include "usage_ui.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// 由 shared/fonts/vn_font_16.c 生成的 GB2312 16px 中文字库。
// 平台名、窗口标签、明细文本都可能是中文,默认的 Montserrat 没有汉字。
LV_FONT_DECLARE(vn_font_16);

// --------------------------------------------------------------------------
// 调色板(docs/usage-monitor-design.md §2)
// --------------------------------------------------------------------------
#define C_BG     0x0B0E0D   // 底:电路板黑绿
#define C_TRACE  0x1F2A24   // 走线:1px 分隔线
#define C_GREY   0x6B7A72   // 丝印灰:次要文字
#define C_WHITE  0xF2F5F3   // 丝印白:主文字
#define C_MINT   0x2EE59D   // 主色:正常 / 活动
#define C_BLUE   0x5B9BFF   // 刷新中
#define C_AMBER  0xFFB454   // 警戒 >=70%
#define C_RED    0xFF5C5C   // 危险 >=90%

// --------------------------------------------------------------------------
// 网格(docs/usage-monitor-design.md §3)
// --------------------------------------------------------------------------
#define SCR_W         240
#define MARGIN_X      6
#define FRAME_TOP     17
#define FRAME_BOTTOM  303
#define PAD           4      // 焊点边长
#define WINDOW_H      60

static lv_obj_t *s_status_dot;
static lv_obj_t *s_status;
static lv_obj_t *s_updated;
static lv_obj_t *s_content;
static lv_obj_t *s_footer;
static lv_obj_t *s_page;

static bool s_detail;

// --------------------------------------------------------------------------
// 颜色语义
// --------------------------------------------------------------------------
static uint32_t severity_color(int percent) {
    if (percent >= 90) return C_RED;
    if (percent >= 70) return C_AMBER;
    return C_MINT;
}

static uint32_t state_color(usage_ui_state_t state) {
    switch (state) {
        case USAGE_UI_LOADING: return C_BLUE;
        case USAGE_UI_OFFLINE: return C_RED;
        default:               return C_MINT;
    }
}

// --------------------------------------------------------------------------
// 母题:焊点与走线
// --------------------------------------------------------------------------
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

// 1px 走线,右端焊一个薄荷绿方块。y 是相对 parent 的坐标。
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

static void clear_content(void) {
    lv_obj_clean(s_content);
}

// --------------------------------------------------------------------------
// 构建
// --------------------------------------------------------------------------
void usage_ui_build(void) {
    lv_obj_t *screen = lv_screen_active();
    // 只设颜色,不动 bg_opa:切换透明度会触发整屏重绘,在直绘场景下有踩坑史。
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // ---- 页眉 ----
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_GREY), 0);
    lv_label_set_text(title, "USAGE MONITOR");
    lv_obj_set_pos(title, MARGIN_X, 4);

    s_status_dot = lv_obj_create(screen);
    lv_obj_set_size(s_status_dot, PAD, PAD);
    lv_obj_set_pos(s_status_dot, 122, 9);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(C_MINT), 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    lv_obj_set_style_radius(s_status_dot, 0, 0);
    lv_obj_set_style_pad_all(s_status_dot, 0, 0);
    lv_obj_set_scrollable(s_status_dot, false);

    s_status = lv_label_create(screen);
    lv_obj_set_style_text_font(s_status, &vn_font_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_MINT), 0);
    lv_label_set_text(s_status, "");
    lv_obj_set_pos(s_status, 130, 2);

    s_updated = lv_label_create(screen);
    lv_obj_set_style_text_font(s_updated, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_updated, lv_color_hex(C_GREY), 0);
    lv_label_set_text(s_updated, "");
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -MARGIN_X, 4);

    // ---- 外框顶线 ----
    add_trace(screen, FRAME_TOP);

    // ---- 内容区 ----
    s_content = lv_obj_create(screen);
    lv_obj_set_size(s_content, SCR_W, FRAME_BOTTOM - FRAME_TOP - 1);
    lv_obj_set_pos(s_content, 0, FRAME_TOP + 1);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_scrollable(s_content, false);

    // ---- 外框底线 ----
    add_trace(screen, FRAME_BOTTOM);

    // ---- 页脚 ----
    s_footer = lv_label_create(screen);
    lv_obj_set_style_text_font(s_footer, &vn_font_16, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(C_GREY), 0);
    lv_label_set_text(s_footer, "");
    lv_obj_set_pos(s_footer, MARGIN_X, FRAME_BOTTOM + 5);

    s_page = lv_label_create(screen);
    lv_obj_set_style_text_font(s_page, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_page, lv_color_hex(C_GREY), 0);
    lv_label_set_text(s_page, "");
    lv_obj_align(s_page, LV_ALIGN_TOP_RIGHT, -MARGIN_X, FRAME_BOTTOM + 5);
}

// --------------------------------------------------------------------------
// 状态栏 / 页脚
// --------------------------------------------------------------------------
void usage_ui_set_state(usage_ui_state_t state, const char *text) {
    const uint32_t color = state_color(state);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    lv_label_set_text(s_status, text);
}

void usage_ui_set_footer(const char *text) {
    lv_label_set_text(s_footer, text);
}

void usage_ui_set_updated(const char *text) {
    lv_label_set_text(s_updated, text);
}

void usage_ui_set_detail(bool enabled) {
    s_detail = enabled;
}

bool usage_ui_detail_enabled(void) {
    return s_detail;
}

void usage_ui_show_message(const char *text) {
    clear_content();

    // 空态也保持骨架:大字 NO DATA + 小字说明,位置和正常页一致。
    lv_obj_t *big = lv_label_create(s_content);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(big, lv_color_hex(C_GREY), 0);
    lv_label_set_text(big, "NO DATA");
    lv_obj_set_pos(big, MARGIN_X, 6);

    add_trace(s_content, 40);

    lv_obj_t *sub = lv_label_create(s_content);
    lv_obj_set_style_text_font(sub, &vn_font_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_GREY), 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sub, SCR_W - MARGIN_X * 2);
    lv_label_set_text(sub, text);
    lv_obj_set_pos(sub, MARGIN_X, 50);
}

// --------------------------------------------------------------------------
// 内容:一个窗口
// --------------------------------------------------------------------------
static void add_window(const usage_window_t *window, int y) {
    const uint32_t color = severity_color(window->used_percent);

    // 第一行:窗口标签(左,小字灰) + 百分比(右,中字按档位着色)。
    lv_obj_t *label = lv_label_create(s_content);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(C_GREY), 0);
    lv_label_set_text(label, window->label);
    lv_obj_set_pos(label, MARGIN_X, y + 4);

    lv_obj_t *percent = lv_label_create(s_content);
    lv_obj_set_style_text_font(percent, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(percent, lv_color_hex(color), 0);
    lv_label_set_text_fmt(percent, "%d%%", window->used_percent);
    lv_obj_align(percent, LV_ALIGN_TOP_RIGHT, -MARGIN_X, y);

    // 第二行:进度条。1px 走线是底色,填充按档位着色;直角,贴合丝印风格。
    lv_obj_t *bar = lv_bar_create(s_content);
    lv_obj_set_size(bar, SCR_W - MARGIN_X * 2, 4);
    lv_obj_set_pos(bar, MARGIN_X, y + 30);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, window->used_percent, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TRACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);

    // 第三行:概览看"多久后重置",详情看服务端给的明细。
    lv_obj_t *third = lv_label_create(s_content);
    lv_obj_set_style_text_font(third, &vn_font_16, 0);
    lv_obj_set_style_text_color(third, lv_color_hex(C_GREY), 0);
    lv_obj_set_width(third, SCR_W - MARGIN_X * 2);
    lv_label_set_long_mode(third, LV_LABEL_LONG_DOT);

    if (s_detail) {
        lv_label_set_text(third, window->detail[0] != '\0' ? window->detail : "—");
    } else {
        char reset[32];
        usage_format_reset(window->resets_in_seconds, reset, sizeof(reset));
        if (window->resets_in_seconds > 0) {
            char line[48];
            snprintf(line, sizeof(line), "%s后重置", reset);
            lv_label_set_text(third, line);
        } else {
            lv_label_set_text(third, "");
        }
    }
    lv_obj_set_pos(third, MARGIN_X, y + 34);

    // 段末走线 + 焊点。
    add_trace(s_content, y + 54);
}

void usage_ui_show(const usage_snapshot_t *snapshot, int index) {
    if (snapshot == NULL || snapshot->platform_count <= 0) {
        usage_ui_show_message("正在获取用量…");
        lv_label_set_text(s_page, "");
        return;
    }

    if (index < 0) index = 0;
    if (index >= snapshot->platform_count) index = snapshot->platform_count - 1;

    clear_content();

    const usage_platform_t *platform = &snapshot->platforms[index];

    // 平台名(大字白) + 页码。
    lv_obj_t *name = lv_label_create(s_content);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(C_WHITE), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, SCR_W - MARGIN_X * 2 - 44);
    lv_label_set_text(name, platform->name);
    lv_obj_set_pos(name, MARGIN_X, 4);

    lv_obj_t *pos = lv_label_create(s_content);
    lv_obj_set_style_text_font(pos, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pos, lv_color_hex(C_GREY), 0);
    lv_label_set_text_fmt(pos, "%d/%d", index + 1, snapshot->platform_count);
    lv_obj_align(pos, LV_ALIGN_TOP_RIGHT, -MARGIN_X, 12);

    add_trace(s_content, 38);

    int y = 42;
    for (int i = 0; i < platform->window_count && i < 4; i++) {
        add_window(&platform->windows[i], y);
        y += WINDOW_H;
    }

    if (platform->window_count == 0) {
        lv_obj_t *empty = lv_label_create(s_content);
        lv_obj_set_style_text_font(empty, &vn_font_16, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(C_GREY), 0);
        lv_label_set_text(empty, "该平台没有数据");
        lv_obj_set_pos(empty, MARGIN_X, y);
    }

    lv_label_set_text_fmt(s_page, "%d/%d", index + 1, snapshot->platform_count);
}
