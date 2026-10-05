// main/usage_model.h —— 用量数据的模型与解析。
//
// 这一层是**纯 C**:不依赖 ESP-IDF、不依赖 LVGL、不碰网络。协议解析最容易
// 出错,把它做成可以在开发机上直接跑单元测试的形式,比"刷机才知道对不对"
// 划算得多(见 tests/test_usage_model.c)。
//
// 协议是紧凑行文本,不是 JSON:设备只有几十 KB 堆,JSON 要 cJSON 加一棵
// 解析树,而行文本用 sscanf 就够,内存占用可以忽略。
//
//   PLATFORMS 2
//   PLATFORM opencode-go OpenCode Go
//   WINDOW 滚动 65 2520 1.3M / 2M tokens
//   WINDOW 每周 30 259200 12M / 40M tokens
//   PLATFORM chatgpt-plus ChatGPT Plus
//   WINDOW 今日 40 3600 40 / 100 条
//   END
//
// 规则:
//   - 平台名与详情取"整行剩余",所以可以含空格;id 与标签是一个词。
//   - 未知行忽略,便于服务端以后加字段而不破坏旧固件。
//   - 百分数会被夹到 0..100,避免脏数据把进度条画到屏外。
#pragma once

#include <stddef.h>

#define USAGE_ID_MAX      24
#define USAGE_NAME_MAX    32
#define USAGE_LABEL_MAX   16
#define USAGE_DETAIL_MAX  48
#define USAGE_MAX_PLATFORMS 8
#define USAGE_MAX_WINDOWS   4

typedef struct {
    char label[USAGE_LABEL_MAX];
    int used_percent;          // 0..100
    int resets_in_seconds;     // 0 = 未知/不适用
    char detail[USAGE_DETAIL_MAX];
} usage_window_t;

typedef struct {
    char id[USAGE_ID_MAX];
    char name[USAGE_NAME_MAX];
    int window_count;
    usage_window_t windows[USAGE_MAX_WINDOWS];
} usage_platform_t;

typedef struct {
    int platform_count;
    usage_platform_t platforms[USAGE_MAX_PLATFORMS];
} usage_snapshot_t;

// 解析成功返回 0;文本为空或没有任何平台则返回非 0。
// out 会被完全覆盖。
int usage_parse(const char *text, usage_snapshot_t *out);

// 把重置倒计时格式化成中文短语,例如 2520 -> "42分", 90000 -> "1天1时"。
// 用于详细模式。纯函数,便于测试。
void usage_format_reset(int seconds, char *buf, size_t size);
