#!/usr/bin/env bash
# The full regression suite. Each suite builds its own temporary workspace so
# they cannot interfere with one another.
#
#   ./test/all.sh
set -u
cd "$(dirname "$0")/.."

HXD="${HX_ENGINE:-../engine/build/hxd}"
fail=0

section() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
report()  { if [ "$1" -eq 0 ]; then printf '\033[32m✓ %s\033[0m\n' "$2"; else printf '\033[31m✗ %s\033[0m\n' "$2"; fail=1; fi; }

section "typecheck"
npx tsc --noEmit; report $? "typecheck"

section "architectural constraints"
./test/arch.sh > /dev/null; report $? "host must not touch fs / start processes"

section "engine: sandbox escape"
(cd ../engine && ./tests/escape/run.sh "build/hxd" > /tmp/hx-escape.out 2>&1); rc=$?
tail -1 /tmp/hx-escape.out; report $rc "escape suite"

section "engine: log durability"
(cd ../engine && python3 tests/durability/kill9.py build/hxd > /tmp/hx-kill9.out 2>&1); rc=$?
tail -2 /tmp/hx-kill9.out; report $rc "kill -9 leaves no half line"

section "engine: interactive PTY session"
(cd ../engine && python3 tests/pty/interactive.py build/hxd > /tmp/hx-pty.out 2>&1); rc=$?
tail -2 /tmp/hx-pty.out; report $rc "pty + sandbox are equally enforced on that path"

section "engine: resource limits and process-tree cleanup"
(cd ../engine && python3 tests/limits/forkbomb.py build/hxd > /tmp/hx-bomb.out 2>&1); rc=$?
tail -2 /tmp/hx-bomb.out; report $rc "neither a fork bomb nor a background daemon leaks"

section "end to end: a multi-step task"
WS=$(mktemp -d /tmp/hx-t-e2e.XXXXXX)
printf 'def add(a, b):\n    return a - b\n' > "$WS/calc.py"
printf 'from calc import add\nassert add(2, 3) == 5\nprint("ALL TESTS PASS")\n' > "$WS/test_calc.py"
npx tsx test/e2e.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "e2e"

section "context compaction"
WS=$(mktemp -d /tmp/hx-t-compact.XXXXXX)
npx tsx test/compaction.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "compaction"

section "policy and approval"
WS=$(mktemp -d /tmp/hx-t-appr.XXXXXX); mkdir -p "$WS/build"
npx tsx test/approval.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "approval"

section "subagents and permission narrowing"
WS=$(mktemp -d /tmp/hx-t-sub.XXXXXX); echo data > "$WS/f.txt"
npx tsx test/subagent.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "subagent"

section "HTTP / SSE service"
WS=$(mktemp -d /tmp/hx-t-srv.XXXXXX); mkdir -p "$WS/build"
npx tsx test/server.ts "$WS" "$HXD" | tail -2; report "${PIPESTATUS[0]}" "server"

printf '\n'
if [ "$fail" -eq 0 ]; then printf '\033[32mall passed\033[0m\n'; else printf '\033[31mfailures present\033[0m\n'; fi
exit $fail
