#!/usr/bin/env bash
# 沙箱逃逸测试套件。每一条都必须"逃不出去"。
#
#   ./tests/escape/run.sh [path-to-hxd]
#
# 退出码：0 = 全部符合预期（含已知缺口按预期存在）；1 = 有回归。
set -u

HXD="${1:-$(dirname "$0")/../../build/hxd}"
WS="$(mktemp -d /tmp/hx-escape.XXXXXX)"
SECRET="$HOME/.codex/auth.json"
[ -r "$SECRET" ] || SECRET="/etc/shadow"

pass=0; fail=0; xfail=0

cleanup() { rm -rf "$WS"; }
trap cleanup EXIT

echo "hello" > "$WS/file.txt"
ln -sf /etc "$WS/etc-link"
ln -sf "$SECRET" "$WS/secret-link"

# check <期望 allow|deny> <描述> -- <命令...>
check() {
  local want="$1" desc="$2"; shift 3
  local out rc
  out=$("$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- "$@" 2>&1)
  rc=$?
  local got="allow"; [ "$rc" -ne 0 ] && got="deny"

  if [ "$want" = "$got" ]; then
    printf '  \033[32mPASS\033[0m  %-44s (%s)\n' "$desc" "$got"
    pass=$((pass+1))
  else
    printf '  \033[31mFAIL\033[0m  %-44s want=%s got=%s rc=%s\n' "$desc" "$want" "$got" "$rc"
    printf '        %s\n' "$(echo "$out" | grep -v '^hxd:' | head -2)"
    fail=$((fail+1))
  fi
}

# xcheck：已知缺口。现在会"逃出去"，补上之后应改成 check。
xcheck() {
  local desc="$1" note="$2"; shift 3
  local rc
  "$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- "$@" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '  \033[33mXFAIL\033[0m %-44s %s\n' "$desc" "$note"
    xfail=$((xfail+1))
  else
    printf '  \033[36mFIXED\033[0m %-44s 缺口已补，请把 xcheck 改成 check\n' "$desc"
    pass=$((pass+1))
  fi
}

echo "hxd: $HXD"
"$HXD" --self-test | head -c 200; echo; echo

MODE=workspace-write; NET=deny
echo "== mode=$MODE net=$NET =="
check allow "读 workspace 内文件"           -- cat "$WS/file.txt"
check allow "写 workspace 内文件"           -- bash -c "echo x > '$WS/new.txt'"
check deny  "读 \$HOME 下的凭据文件"        -- cat "$SECRET"
check deny  "写 /etc/hosts"                 -- bash -c 'echo x > /etc/hosts'
check deny  "经符号链接读凭据"              -- cat "$WS/secret-link"
check deny  "经符号链接写 /etc"             -- bash -c "echo x > '$WS/etc-link/hxtest'"
check deny  "TCP connect"                   -- bash -c 'exec 3<>/dev/tcp/93.184.216.34/80'
check deny  "TCP bind"                      -- python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0))"
check deny  "UDP sendto"                    -- python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.sendto(b'x',('8.8.8.8',53))"
check deny  "IPv6 socket"                   -- python3 -c "import socket;socket.socket(socket.AF_INET6,socket.SOCK_STREAM)"
check allow "AF_UNIX 仍可用（本地通信）"    -- python3 -c "import socket;socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)"
check deny  "mount"                         -- python3 -c "import ctypes,sys;sys.exit(0 if ctypes.CDLL('libc.so.6').mount(b'none',b'/mnt',b'tmpfs',0,None)==0 else 1)"

# ---- 资源上限 ----
# ★ 这里必须同时验两件事，缺一不可：
#   ① 限制确实装上了
#   ② 装上之后正常工作还跑得动
# 只验 ① 曾经让一个严重 bug 全程绿灯通过：RLIMIT_NPROC 写死 256，而它数的是
# 线程不是进程（本机 104 进程 = 667 线程），结果沙箱里每次 fork 都失败。
nproc_limit=$("$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- bash -c 'ulimit -u' 2>/dev/null | tr -d ' ')
fsize_limit=$("$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- bash -c 'ulimit -f' 2>/dev/null | tr -d ' ')
cur_tasks=$(ps -u "$(id -u)" -L --no-headers | wc -l)
if [ "$fsize_limit" = "2097152" ] && { [ "$nproc_limit" = "unlimited" ] || [ "$nproc_limit" -gt "$cur_tasks" ]; }; then
  printf '  \033[32mPASS\033[0m  %-44s (nproc=%s > 当前 %s 线程, fsize=%s)\n' \
    "资源上限已装且留有余量" "$nproc_limit" "$cur_tasks" "$fsize_limit"
  pass=$((pass+1))
else
  printf '  \033[31mFAIL\033[0m  %-44s nproc=%s cur=%s fsize=%s\n' \
    "资源上限已装且留有余量" "$nproc_limit" "$cur_tasks" "$fsize_limit"
  fail=$((fail+1))
fi

# fork 炸弹的测试挪到了 tests/limits/forkbomb.py ——
# 它必须走生产路径（exec.start + timeout_ms）才会 kill(-pgid) 清理进程树。

echo
echo "== 可用性：该放的必须放得通（只验"该拒的拒了"会漏掉整类 bug）=="
check allow "管道 find | head"              -- bash -c "find '$WS' -type f | head -3"
check allow "多级管道 + 子 shell"           -- bash -c "echo a b c | tr ' ' '\\n' | sort | uniq | wc -l"
check allow "命令替换"                      -- bash -c 'x=$(echo hi); test "$x" = hi'
check allow "后台作业 + wait"               -- bash -c '(sleep 0.1; echo done) & wait'
check allow "写临时文件（TMPDIR 可用）"     -- bash -c 'echo x > "$TMPDIR/probe" && cat "$TMPDIR/probe"'
check allow "python 起子进程"               -- python3 -c "import subprocess;subprocess.run(['true'],check=True)"

echo
MODE=read-only; NET=deny
echo "== mode=$MODE net=$NET =="
check allow "读 workspace 内文件"           -- cat "$WS/file.txt"
check deny  "写 workspace 内文件"           -- bash -c "echo x > '$WS/ro.txt'"
check deny  "读 \$HOME 下的凭据文件"        -- cat "$SECRET"

echo
printf 'pass=%d fail=%d xfail(已知缺口)=%d\n' "$pass" "$fail" "$xfail"
[ "$fail" -eq 0 ] || exit 1
