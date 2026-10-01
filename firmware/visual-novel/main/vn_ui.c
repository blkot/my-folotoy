// main/vn_ui.c —— LVGL 视觉小说界面。深色底 + 底部对话框 + 选项列表。
#include "vn_ui.h"

#include "sdkconfig.h"
#include "lvgl.h"

#include <string.h>

// 中文由 assets/fonts/vn_font_16.c 提供(Noto Sans SC, OFL-1.1, GB2312 字集)。
// 非中文字形(图标等)回退到 Montserrat。repos 文档要求:绑定只读字体时保留一个
// 可写描述符,并在任何控件使用它之前初始化一次。
LV_FONT_DECLARE(vn_font_16);

static lv_font_t s_font_with_fallback;
static bool s_font_ready;

#define VN_FONT_TEXT (&s_font_with_fallback)

void vn_ui_fonts_init(void) {
    if (s_font_ready) return;
    s_font_with_fallback = vn_font_16;
    s_font_with_fallback.fallback = &lv_font_montserrat_14;
    s_font_ready = true;
}

bool vn_ui_has_glyph(const lv_font_t *font, uint32_t codepoint) {
    if (font == NULL) return false;
    lv_font_glyph_dsc_t glyph = {0};
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) && !glyph.is_placeholder;
}

#define VN_COL_BG     0x0E1420
#define VN_COL_PANEL  0x1B2438
#define VN_COL_INK    0xE8EEF9
#define VN_COL_ACCENT 0xF2B33D
#define VN_COL_DIM    0x7C8AA5

static lv_obj_t *s_scr;
static lv_obj_t *s_scene;
static lv_obj_t *s_hint;
static lv_obj_t *s_panel;
static lv_obj_t *s_who;
static lv_obj_t *s_text;
static lv_obj_t *s_options[VN_OPT_MAX];

static void options_clear(void) {
    for (int i = 0; i < VN_OPT_MAX; i++) {
        if (s_options[i] != NULL) {
            lv_obj_delete(s_options[i]);
            s_options[i] = NULL;
        }
    }
}

void vn_ui_build(void) {
    vn_ui_fonts_init();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(VN_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_panel, 224, 150);
    lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(VN_COL_PANEL), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    // 必须直角:圆角会让面板四角不被不透明像素覆盖,LVGL 重绘该区域时会把
    // 绘制缓冲里的未定义内容刷到屏幕上。
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_who = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_who, VN_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_who, lv_color_hex(VN_COL_ACCENT), 0);
    lv_obj_align(s_who, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_who, "");

    s_text = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_text, VN_FONT_TEXT, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(VN_COL_INK), 0);
    lv_obj_set_width(s_text, 200);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_label_set_text(s_text, "");

    // 场景名与操作提示也放在不透明面板内部:这样它们重绘时,面板的不透明背景
    // 会先覆盖该区域,不会把绘制缓冲里的未定义内容刷到屏幕上。
    s_scene = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_scene, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_scene, lv_color_hex(VN_COL_DIM), 0);
    lv_obj_align(s_scene, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(s_scene, "");

    s_hint = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(VN_COL_DIM), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(s_hint, "");

    lv_screen_load(s_scr);
}

void vn_ui_destroy(void) {
    options_clear();
    if (s_scr != NULL) {
        lv_obj_delete(s_scr);
    }
    s_scr = NULL;
    s_scene = NULL;
    s_hint = NULL;
    s_panel = NULL;
    s_who = NULL;
    s_text = NULL;
}

void vn_ui_show_node(const vn_node_t *node) {
    if (s_scr == NULL || node == NULL) return;
    options_clear();

    lv_label_set_text(s_who, node->who);
    lv_label_set_text(s_text, node->text);
    if (node->bg[0] != '\0') {
        lv_label_set_text_fmt(s_scene, "SCENE / %s", node->bg);
    } else {
        lv_label_set_text(s_scene, "AI PASSPORT");
    }

    if (vn_node_is_branching(node)) {
        lv_label_set_text(s_hint, "UP/DOWN  OK");
        for (int i = 0; i < node->option_count && i < VN_OPT_MAX; i++) {
            lv_obj_t *row = lv_label_create(s_panel);
            lv_obj_set_style_text_font(row, VN_FONT_TEXT, 0);
            lv_obj_set_width(row, 200);
            lv_label_set_long_mode(row, LV_LABEL_LONG_WRAP);
            const bool selected = (i == node->selected);
            lv_label_set_text_fmt(row, "%s%s", selected ? "> " : "  ", node->options[i].text);
            lv_obj_set_style_text_color(
                row, lv_color_hex(selected ? VN_COL_ACCENT : VN_COL_INK), 0);
            lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 30 + i * 24);
            s_options[i] = row;
        }
    } else {
        lv_label_set_text(s_hint, node->kind == VN_KIND_END ? "OK  restart" : "OK  next");
    }
}

void vn_ui_set_status(const char *text) {
    if (s_scr == NULL) return;
    options_clear();
    lv_label_set_text(s_who, "");
    lv_label_set_text(s_text, text != NULL ? text : "");
    lv_label_set_text(s_hint, "");
}

void vn_ui_repaint(void) {
    // 只重绘对话框面板(及其子对象)。绝不能整屏重绘:screen 背景是透明的,
    // 整屏刷新会把绘制缓冲里的未定义像素写到屏幕上,盖掉直绘的背景。
    if (s_panel == NULL) return;
    lv_obj_invalidate(s_panel);
}
