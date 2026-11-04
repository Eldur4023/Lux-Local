#!/usr/bin/env python3
"""Lux-Local failure-mode tests: host dying, slow/absent host, two instances, shutdown with a request in flight,
garbage on the host protocol. Same sandbox as smoke.py.

  tests/stability.py [--app build/app] [--luxium ../Luxium/build/luxium] [--display :N] [scenario ...]
"""
import argparse, os, signal, socket, stat, sys, time, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smokelib as L
from smokelib import check

ap = argparse.ArgumentParser()
ap.add_argument("--app", default=os.path.join(os.path.dirname(__file__), "..", "build", "app"))
ap.add_argument("--luxium", default=L.LUXIUM_DEFAULT)
ap.add_argument("--display")
ap.add_argument("only", nargs="*")
a = ap.parse_args()
APP, ID = os.path.realpath(a.app), "localify"
xs = L.XServer(a.display)


def sandbox(**kw):
    return L.Sandbox(xs.display, kw.pop("luxium", a.luxium), kw.pop("env", None))


def term(sb, p, what):
    p.send_signal(signal.SIGTERM)
    check(L.wait_gone([p.pid], 20), f"{what}: SIGTERM exits")
    check(L.wait_gone(sb.stray(), 15), f"{what}: nothing left in the sandbox {sb.stray()}")


def host_dies(sig):
    sb = sandbox()
    try:
        p = sb.start(APP)
        check(sb.wait_log(ID, "window opened", 40, p), "window opened")
        h = sb.host_hello()
        os.kill(h["pid"], sig)
        check(sb.wait_log(ID, "lost the connection", 10, p), "app noticed the host went away")
        time.sleep(1.5)
        check(p.poll() is None, "app did not exit")
        check(sb.log(ID).count("window opened") == 2, "window reopened in a fresh host")
        h2 = sb.host_hello()
        check(bool(h2) and h2["pid"] != h["pid"] and h2["windows"] == 1, f"new host holds the window {h2}")
        term(sb, p, "host killed")
    finally: sb.cleanup()


def hostkill(): print("[host SIGKILL mid-session]"); host_dies(signal.SIGKILL)
def hostterm():
    """An orderly host exit either sends `closed` (the app ends like when the user closes) or just drops the
    connection (the app reopens in a fresh host); both are fine, hanging or crashing is not."""
    print("[host SIGTERM mid-session]")
    sb = sandbox()
    try:
        p = sb.start(APP)
        check(sb.wait_log(ID, "window opened", 40, p), "window opened")
        os.kill(sb.host_hello()["pid"], signal.SIGTERM)
        end = time.time() + 10
        while time.time() < end and p.poll() is None and sb.log(ID).count("window opened") < 2: time.sleep(0.2)
        if p.poll() is None:
            check(sb.log(ID).count("window opened") == 2, "app reopened its window in a fresh host")
            term(sb, p, "host terminated")
        else:
            check(p.returncode == 0, f"app ended cleanly (status {p.returncode})")
            check(L.wait_gone(sb.stray(), 15), f"nothing left {sb.stray()}")
    finally: sb.cleanup()


def hostloop():
    """A host that dies every time: after 2 reopen attempts the app falls back to WebKitGTK instead of looping."""
    print("[host keeps dying]")
    sb = sandbox()
    try:
        p = sb.start(APP)
        check(sb.wait_log(ID, "window opened", 40, p), "window opened")
        for _ in range(4):
            h = sb.host_hello()
            if not h: time.sleep(0.3); continue
            os.kill(h["pid"], signal.SIGKILL)
            time.sleep(0.8)
        check(sb.wait_log(ID, "falling back to the WebKitGTK window", 15, p), "gave up on the host and fell back to WebKitGTK")
        check(p.poll() is None, "app still alive")
        term(sb, p, "fallback")
    finally: sb.cleanup()


def slowhost():
    print("[host slow to start]")
    sb = sandbox()
    try:
        wrapper = f"{sb.root}/luxium-slow"
        open(wrapper, "w").write(f"#!/bin/sh\nsleep 4\nexec {a.luxium} \"$@\"\n")
        os.chmod(wrapper, 0o755)
        sb.env["LUXIUM_BIN"] = wrapper
        t = time.time()
        p = sb.start(APP)
        # the wrapper has another size/mtime than the real luxium, so the client also restarts the idle host once
        check(sb.wait_log(ID, "window opened", 40, p), f"window opened after a slow start ({time.time()-t:.1f}s)")
        term(sb, p, "slow host")
    finally: sb.cleanup()


def nohost():
    print("[host binary never creates the socket]")
    sb = sandbox()
    try:
        bad = f"{sb.root}/luxium-bad"
        open(bad, "w").write("#!/bin/sh\nexit 3\n")
        os.chmod(bad, 0o755)
        sb.env["LUXIUM_BIN"] = bad
        t = time.time()
        p = sb.start(APP)
        check(sb.wait_log(ID, "falling back to the WebKitGTK window", 40, p), f"fell back to WebKitGTK ({time.time()-t:.1f}s)")
        check(p.poll() is None and L.port_open(sb.port(ID)), "app and server alive")
        term(sb, p, "no host")
    finally: sb.cleanup()


def twice():
    print("[two instances of the same app]")
    sb = sandbox()
    try:
        p1 = sb.start(APP, tag="1")
        check(sb.wait_log(ID, "window opened", 40, p1), "first instance opened")
        p2 = sb.start(APP, tag="2")
        deadline = time.time() + 40
        while time.time() < deadline and sb.log(ID).count("window opened") < 2 and p2.poll() is None: time.sleep(0.2)
        log = sb.log(ID)
        check(p2.poll() is None, f"second instance did not crash (rc {p2.returncode})")
        check(log.count("window opened") == 2, "both windows opened")
        ports = {int(x) for x in __import__("re").findall(r"server up on port (\d+)", log)}
        check(len(ports) == 2 and all(L.port_open(x) for x in ports), f"two servers {ports}")
        h = sb.host_hello()
        check(bool(h) and h["windows"] == 2, f"one host, two windows {h}")
        for p in (p1, p2): p.send_signal(signal.SIGTERM)
        check(L.wait_gone([p1.pid, p2.pid], 20), "both exit on SIGTERM")
        check(L.wait_gone(sb.stray(), 15), f"nothing left {sb.stray()}")
    finally: sb.cleanup()


def inflight():
    print("[shutdown with requests in flight]")
    sb = sandbox()
    try:
        p = sb.start(APP)
        check(sb.wait_log(ID, "window opened", 40, p), "window opened")
        port = sb.port(ID)
        socks = []
        for body in (b"POST /api/invoke HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n{\"a\":", b"GET /api/settings HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n", b"GET / HT"):
            s = socket.create_connection(("127.0.0.1", port)); s.sendall(body); socks.append(s)
        t = time.time()
        p.send_signal(signal.SIGTERM)
        check(L.wait_gone([p.pid], 20), f"exits with half-sent requests open ({time.time()-t:.1f}s)")
        check(L.wait_gone(sb.stray(), 15), f"nothing left {sb.stray()}")
        for s in socks: s.close()
    finally: sb.cleanup()


FAKE = r'''
import os, socket, sys, time, json
sock, mode = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.bind(sock); s.listen(8)
while True:
    c, _ = s.accept(); f = c.makefile("rwb", buffering=0)
    def say(b): 
        try: c.sendall(b)
        except OSError: pass
    while True:
        l = f.readline()
        if not l: break
        if b'"hello"' in l: say(b'{"ev":"hello","proto":2,"build":"%s","pid":%d,"windows":0,"beat_ms":1}\n' % (os.environ["FAKE_BUILD"].encode(), os.getpid()))
        if b'"ping"' in l: say(b'{"ev":"pong"}\n')
        if b'"open"' in l:
            junk = {
             "garbage": [b'not json\n', b'\xff\xfe\x00\x01\n', b'{"ev":\n', b'{"ev":"resized","width":"abc","height":\n', b'{"ev":"menu","action":"\\u12"}\n', b'{"ev":"result","id":99999999999999999999,"value":"x"}\n', b'\n\n', b'{"ev":"opened"}\n', b'[1,2,3]\n', b'{"ev":"menu"}\n', b'{"ev":"error","message":"\\'+b'"}\n'],
             "huge": [b'{"ev":"opened","x":"' + b"A" * (8 << 20) + b'"}\n', b'{"ev":"opened"}\n'],
             "truncated": [b'{"ev":"opened"}\n', b'{"ev":"resized","width":8'],
            }[mode]
            for j in junk: say(j)
            time.sleep(1)
            if mode == "truncated": c.close(); break
'''


def fake(mode):
    print(f"[host protocol: {mode} lines from the host]")
    sb = sandbox()
    try:
        os.makedirs(sb.rt, mode=0o700, exist_ok=True)
        fp = f"{sb.root}/fake.py"
        open(fp, "w").write(FAKE)
        import subprocess
        st = os.stat("/bin/true")   # any executable: never spawned (the socket answers), only its build id is compared
        h = subprocess.Popen([sys.executable, fp, f"{sb.rt}/host.sock", mode],
                             env=dict(os.environ, FAKE_BUILD=f"{st.st_size}-{int(st.st_mtime)}")); sb.procs.append(h)
        for _ in range(100):
            if os.path.exists(f"{sb.rt}/host.sock"): break
            time.sleep(0.05)
        sb.env["LUXIUM_BIN"] = "/bin/true"
        p = sb.start(APP)
        check(sb.wait_log(ID, "window opened", 20, p), "app got 'opened' through the noise")
        time.sleep(2)
        check(p.poll() is None, f"app survived (rc {p.returncode})")
        check("Sanitizer" not in open(f"{sb.root}/out-a.txt").read(), "no sanitizer report")
        term(sb, p, mode)
    finally: sb.cleanup()


SC = {"hostkill": hostkill, "hostterm": hostterm, "hostloop": hostloop, "slowhost": slowhost, "nohost": nohost,
      "twice": twice, "inflight": inflight, "garbage": lambda: fake("garbage"), "huge": lambda: fake("huge"),
      "truncated": lambda: fake("truncated")}
try:
    for n, f in SC.items():
        if not a.only or n in a.only: f()
finally:
    xs.stop()
sys.exit(L.summary())
