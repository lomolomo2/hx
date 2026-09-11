#!/usr/bin/env python3
"""fork 炸弹必须有界，并且在超时后被连根清理。

★ 这个测试走的是**生产路径**（session.open + exec.start 带 timeout_ms），
  不是 --sandbox-exec。区别很关键：只有生产路径会在超时后 kill(-pgid)
  把整棵进程树收掉。早先这个测试跑在 --sandbox-exec 上，于是"炸弹被挡住"
  的判据变成了看残留进程数少不少 —— 而残留进程恰恰是因为那条路径根本
  不做清理。同一个「测试路径 ≠ 生产路径」的坑，这个项目里踩过三次。

判据也从"造出的进程要少"改成了正确的两条：
  ① 炸弹被上限挡住（bash 报 Resource temporarily unavailable）
  ② 超时后残留清零
造出 ~headroom 个进程本来就是设计预期，不是缺陷。
"""
import json
import subprocess
import sys
import tempfile
import time

HXD = sys.argv[1] if len(sys.argv) > 1 else "build/hxd"


def count_procs() -> int:
    out = subprocess.run(["ps", "-u", str(__import__("os").getuid()), "--no-headers"],
                         capture_output=True, text=True).stdout
    return len([l for l in out.splitlines() if l.strip()])


ws = tempfile.mkdtemp(prefix="hx-bomb.")
p = subprocess.Popen([HXD], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
seq = 0


def call(op, args=None):
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


failures = 0


def check(name, cond, detail=""):
    global failures
    mark = "\x1b[32mPASS\x1b[0m" if cond else "\x1b[31mFAIL\x1b[0m"
    print(f"  {mark}  {name}" + (f" — {detail}" if not cond and detail else ""))
    if not cond:
        failures += 1


before = count_procs()
call("session.open", {"roots": [ws], "sandbox": "workspace-write", "net": "deny", "name": "bomb"})
cell = call("exec.start", {"cmd": ["bash", "-c", ":(){ :|:& };:"], "timeout_ms": 3000})["cell"]

# ★ 按墙钟时间等，不能按循环次数。
#   exec.wait 只要缓冲里有数据就立刻返回，而炸弹让缓冲永远有数据 ——
#   固定 20 次循环几十毫秒就跑完了，根本等不到 3 秒的超时。
out = ""
done = False
deadline = time.time() + 20
while time.time() < deadline:
    r = call("exec.wait", {"cell": cell, "yield_ms": 700, "max_bytes": 4096})
    out += r.get("data") or ""
    if r.get("done"):
        done = True
        break
    time.sleep(0.1)

check("炸弹被资源上限挡住", "Resource temporarily unavailable" in out, repr(out[-120:]))
check("cell 因超时结束", done, "wait 始终没有 done")

# 进程组被收掉需要一点时间
for _ in range(15):
    time.sleep(0.4)
    if count_procs() - before < 20:
        break
residual = count_procs() - before
check("超时后残留清零", residual < 20, f"残留 {residual} 个进程")

# 引擎自己必须活着
check("引擎存活", call("ping").get("pong") is True)

# ---- 更普遍的场景：命令把东西丢到后台就退出 ----
# 这不是炸弹，是日常操作（`npm run dev &`、启个服务）。cell 拥有进程组，
# cell 结束时组必须一起消失，否则 agent 会在系统里悄悄堆下一堆守护进程。
base2 = count_procs()
cell2 = call("exec.start", {"cmd": ["bash", "-c", "(sleep 300 &) ; (sleep 300 &) ; exit 0"],
                            "timeout_ms": 10000})["cell"]
for _ in range(20):
    r = call("exec.wait", {"cell": cell2, "yield_ms": 400})
    if r.get("done"):
        break
for _ in range(10):
    time.sleep(0.4)
    if count_procs() - base2 <= 1:
        break
leaked = count_procs() - base2
check("后台残留的守护进程一并收掉", leaked <= 1, f"漏了 {leaked} 个进程")

print(f"\nfailures={failures} (峰值残留检查基线 {before})")
p.stdin.close()
p.wait(timeout=5)
sys.exit(0 if failures == 0 else 1)
