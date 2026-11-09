#!/usr/bin/env python3
"""Stand-in for mpv in tests: honours --input-ipc-server, plays a fake clock, logs args and commands.
Set VOS_MPV to this file and FAKE_MPV_LOG to a path."""
import json, os, socket, sys, time, select
args = sys.argv[1:]
log = open(os.environ.get("FAKE_MPV_LOG", "/dev/null"), "a")
log.write("ARGS " + json.dumps(args) + "\n"); log.flush()
sock = next((a.split("=", 1)[1] for a in args if a.startswith("--input-ipc-server=")), None)
start = float(next((a.split("=", 1)[1] for a in args if a.startswith("--start=")), 0))
srv = socket.socket(socket.AF_UNIX); 
try: os.unlink(sock)
except Exception: pass
srv.bind(sock); srv.listen(1); srv.settimeout(10)
conn, _ = srv.accept(); conn.setblocking(False)
t0 = time.time(); last = 0; buf = b""; paused = False; pos = start
def send(o): conn.sendall((json.dumps(o) + "\n").encode())
while time.time() - t0 < 25:
    r, _, _ = select.select([conn], [], [], 0.1)
    if r:
        d = conn.recv(4096)
        if not d: break
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try: c = json.loads(line)["command"]
            except Exception: continue
            log.write("CMD " + json.dumps(c) + "\n"); log.flush()
            if c[0] == "observe_property" and c[2] == "duration": send({"event": "property-change", "id": 2, "name": "duration", "data": 3000.0})
            if c[:2] == ["cycle", "pause"]: paused = not paused
            if c[0] == "seek": pos += c[1]
            if c[0] == "quit": log.write("QUIT pos=%.1f\n" % pos); log.flush(); sys.exit(0)
    if time.time() - last > 0.3:
        last = time.time()
        if not paused: pos += 0.3
        send({"event": "property-change", "id": 1, "name": "time-pos", "data": pos})
