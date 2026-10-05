#!/usr/bin/env bash
# 纯逻辑的 host 测试:不需要 ESP-IDF,也不碰硬件。
#
# 这些测试覆盖"可测试的状态机"—— 按仓库约定,协议/时序/布局这类逻辑要与
# ESP-IDF 和 LVGL 解耦,好在开发机上直接跑。
#
# 用法:
#   ./tools/test.sh          # 跑全部
#   ./tools/test.sh vn       # 只跑 VN 引擎
#
# Windows 上请从 MSYS2/MinGW 的 bash 里跑,否则 gcc 可能因缺 DLL 而静默退出。
set -uo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 1

cc="${CC:-gcc}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

failed=0
ran=0

run_c_test() {
    local name="$1"; shift
    ran=$((ran + 1))
    printf '%-28s ' "$name"
    if ! "$cc" -std=c11 -Wall -Wextra -Werror "$@" -o "$tmp/$name" 2>"$tmp/$name.log"; then
        echo "COMPILE FAIL"
        sed 's/^/    /' "$tmp/$name.log"
        failed=$((failed + 1))
        return
    fi
    if "$tmp/$name" >"$tmp/$name.out" 2>&1; then
        echo "PASS"
    else
        echo "FAIL"
        sed 's/^/    /' "$tmp/$name.out"
        failed=$((failed + 1))
    fi
}

want="${1:-all}"

if [[ "$want" == "all" || "$want" == "vn" ]]; then
    run_c_test test_vn_engine \
        -Ifirmware/visual-novel/main \
        firmware/visual-novel/tests/test_vn_engine.c \
        firmware/visual-novel/main/vn_engine.c
fi

if [[ "$want" == "all" || "$want" == "usage" ]]; then
    run_c_test test_usage_model \
        -Ifirmware/usage-monitor/main \
        firmware/usage-monitor/tests/test_usage_model.c \
        firmware/usage-monitor/main/usage_model.c
fi

echo
if [[ $failed -eq 0 ]]; then
    echo "host tests: PASS ($ran suites)"
    exit 0
fi
echo "host tests: FAIL ($failed/$ran suites)"
exit 1
