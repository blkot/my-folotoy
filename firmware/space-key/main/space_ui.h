// main/space_ui.h —— 状态页。
//
// 沿用「本护照」的设计语言(见 docs/usage-monitor-design.md):电路板丝印母题、
// 薄荷绿主色、三档字号、空态保骨架。
#pragma once

#include "space_key.h"

// 建界面。必须在持有 LVGL 锁时调用。
void space_ui_build(void);

// 刷新连接状态(页眉的状态点与文字)。
void space_ui_set_state(space_key_state_t state);

// 已发送次数。
void space_ui_set_count(int count);
