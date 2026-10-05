#include "usage_model.h"

#include <stdio.h>
#include <string.h>

// 复制一行里的"剩余部分"到定长缓冲,并去掉首尾空白。
// 名称与详情允许含空格,所以不能按词切。
static void copy_rest(char *dst, size_t size, const char *src) {
    if (size == 0) return;

    while (*src == ' ' || *src == '\t') src++;

    size_t length = strlen(src);
    while (length > 0 && (src[length - 1] == ' ' || src[length - 1] == '\t' ||
                          src[length - 1] == '\r' || src[length - 1] == '\n')) {
        length--;
    }
    if (length >= size) length = size - 1;

    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void copy_word(char *dst, size_t size, const char *src) {
    if (size == 0) return;

    size_t length = 0;
    while (src[length] != '\0' && src[length] != ' ' && src[length] != '\t' &&
           src[length] != '\r' && src[length] != '\n') {
        length++;
    }
    if (length >= size) length = size - 1;

    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

int usage_parse(const char *text, usage_snapshot_t *out) {
    if (text == NULL || out == NULL) return -1;

    memset(out, 0, sizeof(*out));

    const char *cursor = text;
    usage_platform_t *current = NULL;

    while (*cursor != '\0') {
        // 取一行(不含换行)。
        const char *eol = strchr(cursor, '\n');
        size_t line_length = (eol != NULL) ? (size_t)(eol - cursor) : strlen(cursor);

        char line[256];
        if (line_length >= sizeof(line)) line_length = sizeof(line) - 1;
        memcpy(line, cursor, line_length);
        line[line_length] = '\0';

        if (strncmp(line, "PLATFORM ", 9) == 0) {
            if (out->platform_count < USAGE_MAX_PLATFORMS) {
                current = &out->platforms[out->platform_count];
                out->platform_count++;

                const char *rest = line + 9;
                copy_word(current->id, sizeof(current->id), rest);

                // 跳过 id 与空白,剩下的整段是显示名。
                const char *name = rest;
                while (*name != '\0' && *name != ' ' && *name != '\t') name++;
                copy_rest(current->name, sizeof(current->name), name);

                // 名称为空时退回用 id,界面上至少有东西可显示。
                // 用 memcpy 而不是 snprintf:两者都在 out 里,编译器无法证明
                // 不重叠(restrict),会直接报错。
                if (current->name[0] == '\0') {
                    const size_t id_length = strlen(current->id);
                    const size_t copy_length = (id_length < sizeof(current->name) - 1)
                                                   ? id_length
                                                   : sizeof(current->name) - 1;
                    memcpy(current->name, current->id, copy_length);
                    current->name[copy_length] = '\0';
                }
            } else {
                current = NULL;   // 超出上限:忽略这个平台及其窗口
            }
        } else if (strncmp(line, "WINDOW ", 7) == 0) {
            if (current != NULL && current->window_count < USAGE_MAX_WINDOWS) {
                usage_window_t *window = &current->windows[current->window_count];

                char label[USAGE_LABEL_MAX];
                int percent = 0;
                int resets = 0;

                // 先按词取出前三个字段;详情是整行剩余,不能用 %s 抓。
                if (sscanf(line + 7, "%15s %d %d", label, &percent, &resets) == 3) {
                    copy_word(window->label, sizeof(window->label), label);
                    window->used_percent = clamp_percent(percent);
                    window->resets_in_seconds = resets > 0 ? resets : 0;

                    // 定位到第三个字段之后,把剩余部分当详情。
                    const char *detail = line + 7;
                    int fields = 0;
                    while (*detail != '\0' && fields < 3) {
                        while (*detail == ' ' || *detail == '\t') detail++;
                        while (*detail != '\0' && *detail != ' ' && *detail != '\t') {
                            detail++;
                        }
                        fields++;
                    }
                    copy_rest(window->detail, sizeof(window->detail), detail);

                    current->window_count++;
                }
            }
        }
        // 其余行(含 PLATFORMS / END / 空行 / 未知字段)一律忽略,
        // 这样服务端以后加字段不会让旧固件解析失败。

        if (eol == NULL) break;
        cursor = eol + 1;
    }

    return out->platform_count > 0 ? 0 : -1;
}

void usage_format_reset(int seconds, char *buf, size_t size) {
    if (buf == NULL || size == 0) return;

    if (seconds <= 0) {
        snprintf(buf, size, "--");
        return;
    }

    const int days = seconds / 86400;
    const int hours = (seconds % 86400) / 3600;
    const int minutes = (seconds % 3600) / 60;

    if (days > 0) {
        snprintf(buf, size, "%d天%d时", days, hours);
    } else if (hours > 0) {
        snprintf(buf, size, "%d时%d分", hours, minutes);
    } else if (minutes > 0) {
        snprintf(buf, size, "%d分", minutes);
    } else {
        snprintf(buf, size, "<1分");
    }
}
