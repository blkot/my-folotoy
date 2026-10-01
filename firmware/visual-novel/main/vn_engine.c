// main/vn_engine.c —— 紧凑行协议解析与节点前进规则(纯逻辑,主机可测)。
#include "vn_engine.h"

#include <string.h>

// 在后端保证文本为合法 UTF-8 的前提下,复制时不在多字节字符中间截断。
static void copy_utf8_bounded(char *dst, size_t cap, const char *src, size_t len) {
    if (cap == 0) return;
    if (len > cap - 1) {
        len = cap - 1;
        // 回退到最后一个完整 UTF-8 字符的开头(0b10xxxxxx 为续字节)。
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_bounded(char *dst, size_t cap, const char *src, size_t len) {
    if (cap == 0) return;
    if (len > cap - 1) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static vn_kind_t kind_from_name(const char *name, size_t len) {
    if (len == 3 && strncmp(name, "say", 3) == 0) return VN_KIND_SAY;
    if (len == 3 && strncmp(name, "end", 3) == 0) return VN_KIND_END;
    if (len == 5 && strncmp(name, "scene", 5) == 0) return VN_KIND_SCENE;
    if (len == 6 && strncmp(name, "sprite", 6) == 0) return VN_KIND_SPRITE;
    if (len == 6 && strncmp(name, "choice", 6) == 0) return VN_KIND_CHOICE;
    return VN_KIND_UNKNOWN;
}

int vn_node_parse(const char *wire, vn_node_t *out) {
    if (wire == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->kind = VN_KIND_UNKNOWN;

    bool have_id = false;
    bool have_kind = false;
    const char *cursor = wire;

    while (*cursor != '\0') {
        const char *eol = strchr(cursor, '\n');
        size_t line_len = eol ? (size_t)(eol - cursor) : strlen(cursor);
        if (line_len > 0 && cursor[line_len - 1] == '\r') line_len--;

        if (line_len > 0) {
            size_t key_len = 0;
            while (key_len < line_len && cursor[key_len] != ' ') key_len++;
            size_t value_start = key_len;
            if (value_start < line_len) value_start++;  // skip the single space
            const char *value = cursor + value_start;
            size_t value_len = line_len - value_start;

            if (key_len == 4 && strncmp(cursor, "NODE", 4) == 0) {
                copy_bounded(out->id, sizeof(out->id), value, value_len);
                have_id = true;
            } else if (key_len == 4 && strncmp(cursor, "KIND", 4) == 0) {
                out->kind = kind_from_name(value, value_len);
                have_kind = out->kind != VN_KIND_UNKNOWN;
            } else if (key_len == 3 && strncmp(cursor, "WHO", 3) == 0) {
                copy_utf8_bounded(out->who, sizeof(out->who), value, value_len);
            } else if (key_len == 4 && strncmp(cursor, "TEXT", 4) == 0) {
                copy_utf8_bounded(out->text, sizeof(out->text), value, value_len);
            } else if (key_len == 2 && strncmp(cursor, "BG", 2) == 0) {
                copy_bounded(out->bg, sizeof(out->bg), value, value_len);
            } else if (key_len == 6 && strncmp(cursor, "SPRITE", 6) == 0) {
                copy_bounded(out->sprite, sizeof(out->sprite), value, value_len);
            } else if (key_len == 4 && strncmp(cursor, "NEXT", 4) == 0) {
                copy_bounded(out->next, sizeof(out->next), value, value_len);
            } else if (key_len == 3 && strncmp(cursor, "OPT", 3) == 0) {
                if (out->option_count < VN_OPT_MAX) {
                    size_t id_len = 0;
                    while (id_len < value_len && value[id_len] != ' ') id_len++;
                    size_t text_start = id_len;
                    if (text_start < value_len) text_start++;
                    vn_option_t *option = &out->options[out->option_count];
                    copy_bounded(option->next, sizeof(option->next), value, id_len);
                    copy_utf8_bounded(option->text, sizeof(option->text),
                                      value + text_start, value_len - text_start);
                    out->option_count++;
                }
            }
        }

        if (eol == NULL) break;
        cursor = eol + 1;
    }

    if (!have_id || !have_kind) return -1;
    return 0;
}

const char *vn_node_advance(const vn_node_t *node) {
    if (node == NULL) return NULL;
    switch (node->kind) {
        case VN_KIND_SAY:
        case VN_KIND_SCENE:
        case VN_KIND_SPRITE:
            return node->next[0] != '\0' ? node->next : NULL;
        case VN_KIND_CHOICE:
            if (node->option_count <= 0) return NULL;
            if (node->selected < 0 || node->selected >= node->option_count) return NULL;
            return node->options[node->selected].next[0] != '\0'
                       ? node->options[node->selected].next
                       : NULL;
        default:
            return NULL;
    }
}

void vn_node_select(vn_node_t *node, int delta) {
    if (node == NULL || node->kind != VN_KIND_CHOICE || node->option_count <= 1) return;
    int selected = node->selected + delta;
    while (selected < 0) selected += node->option_count;
    node->selected = selected % node->option_count;
}

bool vn_node_is_branching(const vn_node_t *node) {
    return node != NULL && node->kind == VN_KIND_CHOICE && node->option_count > 0;
}
