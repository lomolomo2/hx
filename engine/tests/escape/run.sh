#!/usr/bin/env bash
# The sandbox escape test suite. Every case must fail to escape.
#
#   ./tests/escape/run.sh [path-to-hxd]
#
# Exit code: 0 = everything as expected (including known gaps still present
# as expected); 1 = a regression.
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

# check <expected allow|deny> <description> -- <command...>
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

# xcheck: a known gap. It escapes today; once closed, change it to check.
xcheck() {
  local desc="$1" note="$2"; shift 3
  local rc
  "$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- "$@" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    printf '  \033[33mXFAIL\033[0m %-44s %s\n' "$desc" "$note"
    xfail=$((xfail+1))
  else
    printf '  \033[36mFIXED\033[0m %-44s gap closed; change xcheck to check\n' "$desc"
    pass=$((pass+1))
  fi
}

echo "hxd: $HXD"
"$HXD" --self-test | head -c 200; echo; echo

MODE=workspace-write; NET=deny
echo "== mode=$MODE net=$NET =="
check allow "read a file inside the workspace"  -- cat "$WS/file.txt"
check allow "write a file inside the workspace" -- bash -c "echo x > '$WS/new.txt'"
check deny  "read a credential under \$HOME"    -- cat "$SECRET"
check deny  "write /etc/hosts"                  -- bash -c 'echo x > /etc/hosts'
check deny  "read a credential via a symlink"   -- cat "$WS/secret-link"
check deny  "write /etc via a symlink"          -- bash -c "echo x > '$WS/etc-link/hxtest'"
check deny  "TCP connect"                   -- bash -c 'exec 3<>/dev/tcp/93.184.216.34/80'
check deny  "TCP bind"                      -- python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0))"
check deny  "UDP sendto"                    -- python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.sendto(b'x',('8.8.8.8',53))"
check deny  "IPv6 socket"                   -- python3 -c "import socket;socket.socket(socket.AF_INET6,socket.SOCK_STREAM)"
check allow "AF_UNIX still works (local IPC)"   -- python3 -c "import socket;socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)"
check deny  "mount"                         -- python3 -c "import ctypes,sys;sys.exit(0 if ctypes.CDLL('libc.so.6').mount(b'none',b'/mnt',b'tmpfs',0,None)==0 else 1)"

# ---- resource limits ----
# ★ Two things must be verified here, and neither can be skipped:
#   1. the limit really was applied
#   2. ordinary work still runs once it is
# Verifying only 1 once let a serious bug pass green the whole way through:
# RLIMIT_NPROC was hardcoded to 256 while it counts threads rather than
# processes (on this machine 104 processes = 667 threads), so every fork inside
# the sandbox failed.
nproc_limit=$("$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- bash -c 'ulimit -u' 2>/dev/null | tr -d ' ')
fsize_limit=$("$HXD" --sandbox-exec --mode "$MODE" --net "$NET" --root "$WS" -- bash -c 'ulimit -f' 2>/dev/null | tr -d ' ')
cur_tasks=$(ps -u "$(id -u)" -L --no-headers | wc -l)
if [ "$fsize_limit" = "2097152" ] && { [ "$nproc_limit" = "unlimited" ] || [ "$nproc_limit" -gt "$cur_tasks" ]; }; then
  printf '  \033[32mPASS\033[0m  %-44s (nproc=%s > current %s threads, fsize=%s)\n' \
    "resource limits applied, with headroom" "$nproc_limit" "$cur_tasks" "$fsize_limit"
  pass=$((pass+1))
else
  printf '  \033[31mFAIL\033[0m  %-44s nproc=%s cur=%s fsize=%s\n' \
    "resource limits applied, with headroom" "$nproc_limit" "$cur_tasks" "$fsize_limit"
  fail=$((fail+1))
fi

# The fork bomb test moved to tests/limits/forkbomb.py -- it has to take the
# production path (exec.start + timeout_ms) for kill(-pgid) to clean up the
# process tree.

echo
echo "== usability: what should pass must get through (verifying only that the forbidden was refused misses a whole class of bugs) =="
check allow "pipe: find | head"                 -- bash -c "find '$WS' -type f | head -3"
check allow "multi-stage pipe + subshell"       -- bash -c "echo a b c | tr ' ' '\\n' | sort | uniq | wc -l"
check allow "command substitution"              -- bash -c 'x=$(echo hi); test "$x" = hi'
check allow "background job + wait"             -- bash -c '(sleep 0.1; echo done) & wait'
check allow "write a temp file (TMPDIR usable)" -- bash -c 'echo x > "$TMPDIR/probe" && cat "$TMPDIR/probe"'
check allow "python starts a child process"     -- python3 -c "import subprocess;subprocess.run(['true'],check=True)"

echo
MODE=read-only; NET=deny
echo "== mode=$MODE net=$NET =="
check allow "read a file inside the workspace"  -- cat "$WS/file.txt"
check deny  "write a file inside the workspace" -- bash -c "echo x > '$WS/ro.txt'"
check deny  "read a credential under \$HOME"    -- cat "$SECRET"

echo
printf 'pass=%d fail=%d xfail(known gaps)=%d\n' "$pass" "$fail" "$xfail"
[ "$fail" -eq 0 ] || exit 1
