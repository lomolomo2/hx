#!/usr/bin/env python3
"""M4 判据：PTY 会话能跑 REPL，且沙箱在这条路径上同样生效。

★ 为什么要单独验 PTY 的沙箱：
  pty 是与管道版不同的另一条 spawn 路径。安全属性不会自动继承 ——
  两条路径必须各自证明，否则就会出现"管道版拦住了、pty 版漏了"这种事。
"""
import json
import os
import subprocess
import sys
import tempfile
import time

HXD = sys.argv[1] if len(sys.argv) > 1 else "build/hxd"

# 真实家目录里一个必然存在、而沙箱不该放行的文件
HOME_PROBE = os.path.expanduser("~/.bashrc")

ws = tempfile.mkdtemp(prefix="hx-pty.")
with open(f"{ws}/data.txt", "w") as f:
    f.write("workspace-file\n")

p = subprocess.Popen([HXD], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1)

seq = 0
failures = 0


def call(op, args=None):
    """发一个请求，读到它的响应为止（中间的事件先跳过）。"""
    global seq
    seq += 1
    rid = f"r{seq}"
    p.stdin.write(json.dumps({"id": rid, "op": op, "args": args or {}}) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            raise RuntimeError("engine closed")
        msg = json.loads(line)
        if msg.get("reply_to") == rid:
            if not msg.get("ok"):
                raise RuntimeError(f"{op} failed: {msg.get('error')}")
            return msg.get("result", {})


def drain(cell, want, tries=12):
    """反复 wait，直到看到想要的片段或次数用尽。"""
    acc = ""
    for _ in range(tries):
        r = call("exec.wait", {"cell": cell, "yield_ms": 600, "max_bytes": 8192})
        acc += r.get("data") or ""
        if want in acc or r.get("done"):
            break
        time.sleep(0.05)
    return acc


def check(name, cond, detail=""):
    global failures
    mark = "\x1b[32mPASS\x1b[0m" if cond else "\x1b[31mFAIL\x1b[0m"
    print(f"  {mark}  {name}" + (f" — {detail}" if not cond and detail else ""))
    if not cond:
        failures += 1


call("session.open", {"roots": [ws], "sandbox": "workspace-write", "net": "deny", "name": "ptyi"})
started = call("exec.start", {"cmd": ["python3", "-i", "-q"], "pty": True, "timeout_ms": 60000})
cell = started["cell"]
check("pty 会话已建立", started.get("pty") is True and bool(cell), json.dumps(started))

banner = drain(cell, ">>>")
check("拿到 REPL 提示符", ">>>" in banner, repr(banner[:80]))

call("exec.stdin", {"cell": cell, "data": "2+3\n"})
out = drain(cell, "5")
check("REPL 求值正确", "5" in out, repr(out[:120]))

call("exec.stdin", {"cell": cell, "data": 'open("data.txt").read()\n'})
out = drain(cell, "workspace-file")
check("能读 workspace 内文件", "workspace-file" in out, repr(out[:120]))

# ★ 关键：pty 这条路径上沙箱必须同样生效。
#
# 探针路径必须在**驱动侧**（沙箱外）算好再传进去：沙箱里的 HOME 被指向了
# 工作区根目录，在里面展开 "~" 只会得到"文件不存在"，测不出"权限被拒"。
call("exec.stdin", {"cell": cell, "data": f'open({HOME_PROBE!r}).read()\n'})
out = drain(cell, "Error")
check("pty 内读 $HOME 下的文件被拒", "PermissionError" in out or "Permission denied" in out, repr(out[:160]))

# 网络同理
call("exec.stdin", {"cell": cell, "data": "import socket; socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"})
out = drain(cell, "Error")
check("pty 内建 UDP socket 被拒", "PermissionError" in out or "not permitted" in out, repr(out[:160]))

call("exec.stdin", {"cell": cell, "data": "exit()\n"})
tail = drain(cell, "", tries=10)
final = call("exec.wait", {"cell": cell, "yield_ms": 2000})
check("退出后 cell 收尾", final.get("done") is True and final.get("exit_code") == 0,
      json.dumps({k: final.get(k) for k in ("done", "exit_code")}))

print(f"\nfailures={failures}")
p.stdin.close()
p.wait(timeout=5)
sys.exit(0 if failures == 0 else 1)
