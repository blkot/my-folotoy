// tests/test_usage_model.c —— 用量协议解析的单元测试。
//
// 解析是这条链路里最容易出错的一环(整行剩余、空格、脏数据),而它又是纯逻辑,
// 所以做成不依赖硬件的测试:改完直接 ./tools/test.sh 就知道对不对。
#include "usage_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_basic(void) {
    const char *text =
        "PLATFORMS 2\n"
        "PLATFORM opencode-go OpenCode Go\n"
        "WINDOW 滚动 65 2520 1.3M / 2M tokens\n"
        "WINDOW 每周 30 259200 12M / 40M tokens\n"
        "PLATFORM chatgpt-plus ChatGPT Plus\n"
        "WINDOW 今日 40 3600 40 / 100 条\n"
        "END\n";

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);

    assert(snapshot.platform_count == 2);

    assert(strcmp(snapshot.platforms[0].id, "opencode-go") == 0);
    assert(strcmp(snapshot.platforms[0].name, "OpenCode Go") == 0);
    assert(snapshot.platforms[0].window_count == 2);

    assert(strcmp(snapshot.platforms[0].windows[0].label, "滚动") == 0);
    assert(snapshot.platforms[0].windows[0].used_percent == 65);
    assert(snapshot.platforms[0].windows[0].resets_in_seconds == 2520);
    assert(strcmp(snapshot.platforms[0].windows[0].detail, "1.3M / 2M tokens") == 0);

    assert(strcmp(snapshot.platforms[1].id, "chatgpt-plus") == 0);
    assert(strcmp(snapshot.platforms[1].name, "ChatGPT Plus") == 0);
    assert(snapshot.platforms[1].window_count == 1);
    assert(strcmp(snapshot.platforms[1].windows[0].detail, "40 / 100 条") == 0);
}

// 名称与详情里的空格必须原样保留 —— 这是"整行剩余"最容易写错的地方。
static void test_spaces_preserved(void) {
    const char *text =
        "PLATFORM foo My Long Platform Name\n"
        "WINDOW 本月 12 100 48M tokens used of 400M\n";

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(strcmp(snapshot.platforms[0].name, "My Long Platform Name") == 0);
    assert(strcmp(snapshot.platforms[0].windows[0].detail,
                  "48M tokens used of 400M") == 0);
}

// 脏数据不能让进度条画到屏外。
static void test_percent_clamped(void) {
    const char *text =
        "PLATFORM p P\n"
        "WINDOW a 150 0 over\n"
        "WINDOW b -20 0 under\n";

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(snapshot.platforms[0].windows[0].used_percent == 100);
    assert(snapshot.platforms[0].windows[1].used_percent == 0);
    // 负数重置时间按"未知"处理。
    assert(snapshot.platforms[0].windows[0].resets_in_seconds == 0);
}

// 服务端以后加字段不应让旧固件失败。
static void test_unknown_lines_ignored(void) {
    const char *text =
        "SCHEMA 2\n"
        "PLATFORM p P\n"
        "SOMETHING new field\n"
        "WINDOW a 10 60 d\n"
        "TRAILER whatever\n";

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(snapshot.platform_count == 1);
    assert(snapshot.platforms[0].window_count == 1);
}

// 超出上限时截断而不是溢出。
static void test_overflow_truncated(void) {
    char text[4096];
    int offset = 0;
    offset += snprintf(text + offset, sizeof(text) - offset, "PLATFORMS 20\n");
    for (int i = 0; i < 20; i++) {
        offset += snprintf(text + offset, sizeof(text) - offset,
                           "PLATFORM p%d Platform %d\n", i, i);
        for (int w = 0; w < 8; w++) {
            offset += snprintf(text + offset, sizeof(text) - offset,
                               "WINDOW w%d 10 60 d\n", w);
        }
    }

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(snapshot.platform_count == USAGE_MAX_PLATFORMS);
    for (int i = 0; i < snapshot.platform_count; i++) {
        assert(snapshot.platforms[i].window_count <= USAGE_MAX_WINDOWS);
    }
}

// 窗口出现在任何平台之前时不能崩,也不该挂到别的平台上。
static void test_window_before_platform(void) {
    const char *text =
        "WINDOW stray 10 60 d\n"
        "PLATFORM p P\n"
        "WINDOW ok 20 60 d\n";

    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(snapshot.platform_count == 1);
    assert(snapshot.platforms[0].window_count == 1);
    assert(strcmp(snapshot.platforms[0].windows[0].label, "ok") == 0);
}

static void test_empty_and_garbage(void) {
    usage_snapshot_t snapshot;
    assert(usage_parse("", &snapshot) != 0);
    assert(usage_parse("PLATFORMS 0\nEND\n", &snapshot) != 0);
    assert(usage_parse(NULL, &snapshot) != 0);
}

// 名称缺失时退回 id,界面上至少有东西可显示。
static void test_missing_name_falls_back_to_id(void) {
    const char *text = "PLATFORM only-id\n";
    usage_snapshot_t snapshot;
    assert(usage_parse(text, &snapshot) == 0);
    assert(strcmp(snapshot.platforms[0].name, "only-id") == 0);
}

static void test_reset_formatting(void) {
    char buf[32];

    usage_format_reset(0, buf, sizeof(buf));
    assert(strcmp(buf, "--") == 0);

    usage_format_reset(30, buf, sizeof(buf));
    assert(strcmp(buf, "<1分") == 0);

    usage_format_reset(2520, buf, sizeof(buf));      // 42 分钟
    assert(strcmp(buf, "42分") == 0);

    usage_format_reset(3600 + 1200, buf, sizeof(buf));   // 1 时 20 分
    assert(strcmp(buf, "1时20分") == 0);

    usage_format_reset(86400 + 3600, buf, sizeof(buf));  // 1 天 1 时
    assert(strcmp(buf, "1天1时") == 0);
}

int main(void) {
    test_basic();
    test_spaces_preserved();
    test_percent_clamped();
    test_unknown_lines_ignored();
    test_overflow_truncated();
    test_window_before_platform();
    test_empty_and_garbage();
    test_missing_name_falls_back_to_id();
    test_reset_formatting();

    printf("usage_model tests: PASS\n");
    return 0;
}
