#!/usr/bin/env python3
"""M3 判据：写日志过程中 kill -9，日志里不能出现半行 JSON。

这验证的是「一次 write() 追加」的原子性承诺。
"""
import json, os, pathlib, signal, subprocess, sys, tempfile, time

HXD = sys.argv[1] if len(sys.argv) > 1 else "build/hxd"
ws = tempfile.mkdtemp(prefix="hx-kill9.")
name = "kill9test"

p = subprocess.Popen([HXD], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
send = lambda o: (p.stdin.write((json.dumps(o) + "\n").encode()), p.stdin.flush())

send({"id": "s", "op": "session.open",
      "args": {"roots": [ws], "sandbox": "workspace-write", "name": name}})
time.sleep(0.3)

# 灌大量记录，中途强杀
payload = {"type": "response_item", "payload": {"type": "message", "text": "x" * 900}}
n = 0
try:
    deadline = time.time() + 0.7
    while time.time() < deadline:
        send({"id": f"l{n}", "op": "log.append", "args": {"record": payload}})
        n += 1
except BrokenPipeError:
    pass

os.kill(p.pid, signal.SIGKILL)
p.wait()
print(f"sent {n} records, then SIGKILL")

logs = sorted(pathlib.Path(os.path.expanduser("~/.hx/sessions")).rglob(f"rollout-{name}.jsonl"),
              key=lambda q: q.stat().st_mtime)
if not logs:
    print("FAIL: no rollout written"); sys.exit(1)
log = logs[-1]

good = bad = 0
seqs = []
with open(log, "rb") as f:
    raw = f.read()
for i, line in enumerate(raw.split(b"\n")):
    if not line:
        continue
    try:
        d = json.loads(line)
        seqs.append(d["seq"])
        good += 1
    except Exception as e:
        bad += 1
        print(f"  PARTIAL LINE at index {i}: {line[:80]!r} ({e})")

trailing_ok = raw.endswith(b"\n") or not raw
contiguous = seqs == list(range(1, len(seqs) + 1))

print(f"log: {log}")
print(f"lines parsed ok = {good}, partial = {bad}")
print(f"ends with newline = {trailing_ok}, seq contiguous 1..{len(seqs)} = {contiguous}")
os.unlink(log)
sys.exit(0 if (bad == 0 and trailing_ok and contiguous and good > 10) else 1)
