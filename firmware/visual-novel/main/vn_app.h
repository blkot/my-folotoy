// main/vn_app.h —— 视觉小说应用的编排层(联网 + 工作任务 + 按键)。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

// 连接 Wi-Fi、创建界面与工作任务。返回非 ESP_OK 时调用方应回退到基线菜单。
esp_err_t vn_app_start(void);

// 由输入任务调用:只把事件入队,不做任何阻塞或 LVGL 访问。
void vn_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 用户长按「确定」请求离开 VN 应用(回到开发者菜单)。
bool vn_app_should_exit(void);

// 停止工作任务并销毁界面。必须在 vn_app_should_exit() 为真后调用。
void vn_app_stop(void);
