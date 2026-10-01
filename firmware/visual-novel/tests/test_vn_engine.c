// tests/test_vn_engine.c —— 视觉小说纯逻辑层的主机测试(不依赖 ESP-IDF/LVGL)。
#include "vn_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_parse_say(void) {
    vn_node_t node;
    const char *wire = "NODE start_2\nKIND say\nTEXT 天快黑了。\nNEXT start_3\n\n";
    assert(vn_node_parse(wire, &node) == 0);
    assert(node.kind == VN_KIND_SAY);
    assert(strcmp(node.id, "start_2") == 0);
    assert(strcmp(node.text, "天快黑了。") == 0);
    assert(strcmp(node.next, "start_3") == 0);
    assert(node.option_count == 0);
    assert(strcmp(vn_node_advance(&node), "start_3") == 0);
}

static void test_parse_say_with_speaker(void) {
    vn_node_t node;
    const char *wire = "NODE n1\nKIND say\nWHO 旁白\nTEXT 你要先看哪一件？\nNEXT n2\n";
    assert(vn_node_parse(wire, &node) == 0);
    assert(strcmp(node.who, "旁白") == 0);
    assert(strcmp(node.text, "你要先看哪一件？") == 0);
}

static void test_parse_scene(void) {
    vn_node_t node;
    assert(vn_node_parse("NODE start\nKIND scene\nBG bg_hall\nNEXT start_2\n", &node) == 0);
    assert(node.kind == VN_KIND_SCENE);
    assert(strcmp(node.bg, "bg_hall") == 0);
    assert(strcmp(vn_node_advance(&node), "start_2") == 0);
}

static void test_parse_choice_and_select(void) {
    vn_node_t node;
    const char *wire =
        "NODE start_5\nKIND choice\n"
        "OPT watch_1 会发光的怀表\n"
        "OPT mirror_1 不会反光的镜子\n\n";
    assert(vn_node_parse(wire, &node) == 0);
    assert(node.kind == VN_KIND_CHOICE);
    assert(node.option_count == 2);
    assert(node.selected == 0);
    assert(strcmp(node.options[0].text, "会发光的怀表") == 0);
    assert(strcmp(node.options[1].next, "mirror_1") == 0);
    assert(strcmp(vn_node_advance(&node), "watch_1") == 0);

    vn_node_select(&node, 1);
    assert(node.selected == 1);
    assert(strcmp(vn_node_advance(&node), "mirror_1") == 0);

    vn_node_select(&node, 1);   // wraps back to the first option
    assert(node.selected == 0);
    vn_node_select(&node, -1);  // wraps to the last option
    assert(node.selected == 1);
}

static void test_end_advances_nowhere(void) {
    vn_node_t node;
    assert(vn_node_parse("NODE watch_3\nKIND end\nTEXT —— 完 ——\n", &node) == 0);
    assert(node.kind == VN_KIND_END);
    assert(vn_node_advance(&node) == NULL);
}

static void test_rejects_missing_kind(void) {
    vn_node_t node;
    assert(vn_node_parse("TEXT only text\n", &node) == -1);
    assert(vn_node_parse("NODE n1\nKIND bogus\n", &node) == -1);
}

static void test_truncates_on_utf8_boundary(void) {
    // VN_TEXT_MAX - 2 ASCII bytes, followed by one 3-byte character that does
    // not fit: the partial character must be dropped, not split.
    char wire[512];
    char expected[VN_TEXT_MAX];
    memset(expected, 'A', VN_TEXT_MAX - 2);
    expected[VN_TEXT_MAX - 2] = '\0';

    int offset = snprintf(wire, sizeof(wire), "NODE n1\nKIND say\nTEXT ");
    memset(wire + offset, 'A', VN_TEXT_MAX - 2);
    offset += VN_TEXT_MAX - 2;
    offset += snprintf(wire + offset, sizeof(wire) - (size_t)offset, "\xE4\xB8\xAD\n");  // 中
    (void)snprintf(wire + offset, sizeof(wire) - (size_t)offset, "\n");

    vn_node_t node;
    assert(vn_node_parse(wire, &node) == 0);
    assert(strcmp(node.text, expected) == 0);
}

static void test_parse_sprite(void) {
    vn_node_t node;
    assert(vn_node_parse("NODE watch_2\nKIND sprite\nSPRITE hero\nNEXT watch_3\n", &node) == 0);
    assert(node.kind == VN_KIND_SPRITE);
    assert(strcmp(node.sprite, "hero") == 0);
    assert(strcmp(vn_node_advance(&node), "watch_3") == 0);

    // An empty SPRITE line means "hide the sprite" and must still advance.
    assert(vn_node_parse("NODE mirror_1\nKIND sprite\nNEXT mirror_2\n", &node) == 0);
    assert(node.kind == VN_KIND_SPRITE);
    assert(node.sprite[0] == '\0');
    assert(strcmp(vn_node_advance(&node), "mirror_2") == 0);
}

static void test_kind_name_helpers(void) {
    vn_node_t node;
    assert(vn_node_parse("NODE c\nKIND choice\nOPT a 甲\n", &node) == 0);
    assert(vn_node_is_branching(&node));
    assert(vn_node_parse("NODE s\nKIND say\nTEXT x\n", &node) == 0);
    assert(!vn_node_is_branching(&node));
}

int main(void) {
    test_parse_say();
    test_parse_say_with_speaker();
    test_parse_scene();
    test_parse_choice_and_select();
    test_end_advances_nowhere();
    test_rejects_missing_kind();
    test_parse_sprite();
    test_truncates_on_utf8_boundary();
    test_kind_name_helpers();
    printf("vn_engine tests: PASS\n");
    return 0;
}
