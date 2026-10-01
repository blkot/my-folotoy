// main/vn_ui.h —— 视觉小说界面(全新设计,不复用基线 demo 外壳)。
// 所有函数都必须在 LVGL 任务中或持有 bsp_lvgl_lock() 时调用。
#pragma once

#include "lvgl.h"
#include "vn_engine.h"

// 创建并载入 VN 屏幕。
void vn_ui_build(void);

// 初始化字体(可写描述符 + fallback)。任何控件使用前调用一次,幂等。
void vn_ui_fonts_init(void);

// 检查某个 Unicode 码点是否有字形(用于缺字排查,不用于正常绘制)。
bool vn_ui_has_glyph(const lv_font_t *font, uint32_t codepoint);

// 请求重绘对话框面板(及其子对象)。背景由 vn_bg 直绘到面板,LVGL 只重绘
// 这个不透明面板,才不会覆盖直绘背景。
void vn_ui_repaint(void);

// 删除 VN 屏幕并清空所有对象指针。
void vn_ui_destroy(void);

// 按当前节点刷新台词/说话人/选项。
void vn_ui_show_node(const vn_node_t *node);

// 显示一条状态文本(连接中/错误提示等)。
void vn_ui_set_status(const char *text);
