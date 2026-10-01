// main/vn_engine.h —— 视觉小说运行时的纯逻辑层。
//
// 只做「紧凑行协议 -> 节点模型」的解析和前进/选择规则,不依赖 ESP-IDF、LVGL
// 或网络,因此可以在主机上单独编译并测试(tests/test_vn_engine.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VN_ID_MAX        24
#define VN_WHO_MAX       24
#define VN_TEXT_MAX      192
#define VN_OPT_MAX       4
#define VN_OPT_TEXT_MAX  48

typedef enum {
    VN_KIND_UNKNOWN = 0,
    VN_KIND_SAY,     // 一句台词(可带说话人)
    VN_KIND_SCENE,   // 切换场景;记录 bg 并自动前进
    VN_KIND_SPRITE,  // 显示/隐藏立绘(空 sprite 表示隐藏),记录并自动前进
    VN_KIND_CHOICE,  // 单选分支
    VN_KIND_END,     // 结束
} vn_kind_t;

typedef struct {
    char next[VN_ID_MAX];
    char text[VN_OPT_TEXT_MAX];
} vn_option_t;

typedef struct {
    vn_kind_t kind;
    char id[VN_ID_MAX];
    char who[VN_WHO_MAX];
    char text[VN_TEXT_MAX];
    char bg[VN_ID_MAX];
    char sprite[VN_ID_MAX];
    char next[VN_ID_MAX];
    vn_option_t options[VN_OPT_MAX];
    int option_count;
    int selected;
} vn_node_t;

// 解析后端下发的紧凑行协议。成功返回 0;缺少 NODE/KIND 或字段越界返回 -1。
// 文本字段按 UTF-8 原样存放,长度超出缓冲区时在字符边界截断。
int vn_node_parse(const char *wire, vn_node_t *out);

// 当前节点「确定」后应跳转到的节点 id:
//   say/scene -> next;choice -> 当前选中项;end/无目标 -> NULL。
const char *vn_node_advance(const vn_node_t *node);

// 在 choice 内移动选中项(delta 为 ±1),循环包裹。
void vn_node_select(vn_node_t *node, int delta);

// 是否是等待用户选择的分支节点。
bool vn_node_is_branching(const vn_node_t *node);
