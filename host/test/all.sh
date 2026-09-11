#!/usr/bin/env bash
# 全量回归。每个套件自建临时工作区，互不干扰。
#
#   ./test/all.sh
set -u
cd "$(dirname "$0")/.."

HXD="${HX_ENGINE:-../engine/build/hxd}"
fail=0

section() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
report()  { if [ "$1" -eq 0 ]; then printf '\033[32m✓ %s\033[0m\n' "$2"; else printf '\033[31m✗ %s\033[0m\n' "$2"; fail=1; fi; }

section "类型检查"
npx tsc --noEmit; report $? "typecheck"

section "架构约束"
./test/arch.sh > /dev/null; report $? "host 不得直接碰 fs / 起进程"

section "引擎：沙箱逃逸"
(cd ../engine && ./tests/escape/run.sh "build/hxd" > /tmp/hx-escape.out 2>&1); rc=$?
tail -1 /tmp/hx-escape.out; report $rc "escape suite"

section "引擎：日志持久性"
(cd ../engine && python3 tests/durability/kill9.py build/hxd > /tmp/hx-kill9.out 2>&1); rc=$?
tail -2 /tmp/hx-kill9.out; report $rc "kill -9 不留半行"

section "引擎：PTY 交互会话"
(cd ../engine && python3 tests/pty/interactive.py build/hxd > /tmp/hx-pty.out 2>&1); rc=$?
tail -2 /tmp/hx-pty.out; report $rc "pty + 沙箱在该路径同样生效"

section "引擎：资源上限与进程树清理"
(cd ../engine && python3 tests/limits/forkbomb.py build/hxd > /tmp/hx-bomb.out 2>&1); rc=$?
tail -2 /tmp/hx-bomb.out; report $rc "fork 炸弹 + 后台守护进程都不外泄"

section "端到端：多步任务"
WS=$(mktemp -d /tmp/hx-t-e2e.XXXXXX)
printf 'def add(a, b):\n    return a - b\n' > "$WS/calc.py"
printf 'from calc import add\nassert add(2, 3) == 5\nprint("ALL TESTS PASS")\n' > "$WS/test_calc.py"
npx tsx test/e2e.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "e2e"

section "上下文压缩"
WS=$(mktemp -d /tmp/hx-t-compact.XXXXXX)
npx tsx test/compaction.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "compaction"

section "策略与审批"
WS=$(mktemp -d /tmp/hx-t-appr.XXXXXX); mkdir -p "$WS/build"
npx tsx test/approval.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "approval"

section "子 agent 与权限收窄"
WS=$(mktemp -d /tmp/hx-t-sub.XXXXXX); echo data > "$WS/f.txt"
npx tsx test/subagent.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "subagent"

section "HTTP / SSE 服务"
WS=$(mktemp -d /tmp/hx-t-srv.XXXXXX); mkdir -p "$WS/build"
npx tsx test/server.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "server"

printf '\n'
if [ "$fail" -eq 0 ]; then printf '\033[32m全部通过\033[0m\n'; else printf '\033[31m有失败项\033[0m\n'; fi
exit $fail
