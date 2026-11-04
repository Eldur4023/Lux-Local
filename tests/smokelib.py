"""Smoke-test helpers for Lux-Local style apps (the same file is vendored into homeflix/desktop/tests).

Everything runs in a private sandbox: temp HOME/XDG_*, its own Luxium host socket, its own X server (Xvfb or
Xephyr, started here). Only processes started by the test are ever signalled.
"""
import json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error

LUXIUM_DEFAULT = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", "..", "Luxium", "build", "luxium"))
_fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond: _fails.append(msg)
    return cond


def summary():
    print(f"\n{len(_fails)} failure(s)" if _fails else "\nall passed")
    return 1 if _fails else 0


class XServer:
    """A private X server, so the windows never reach the user's desktop session when Xvfb exists."""
    def __init__(self, display=None):
        self.proc = None
        self.display = display
        if display: return
        for n in range(97, 80, -1):
            if os.path.exists(f"/tmp/.X11-unix/X{n}"): continue
            exe = shutil.which("Xvfb")
            argv = [exe, f":{n}", "-screen", "0", "1280x800x24"] if exe else \
                   [shutil.which("Xephyr") or "", f":{n}", "-screen", "1280x800", "-ac"] if shutil.which("Xephyr") else None
            if not argv: break
            self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(100):
                if os.path.exists(f"/tmp/.X11-unix/X{n}"): break
                time.sleep(0.05)
            self.display = f":{n}"
            return
        self.display = os.environ.get("DISPLAY", ":1")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try: self.proc.wait(5)
            except subprocess.TimeoutExpired: self.proc.kill()


class Sandbox:
    def __init__(self, display, luxium=LUXIUM_DEFAULT, extra_env=None):
        self.root = tempfile.mkdtemp(prefix="smk-")   # short: the host socket path must stay under 108 bytes
        r = self.root
        for d in ("r", "h", "s", "d", "c"): os.makedirs(f"{r}/{d}", mode=0o700, exist_ok=True)
        self.rt = f"{r}/r/luxium"
        self.env = dict(os.environ, HOME=f"{r}/h", XDG_RUNTIME_DIR=f"{r}/r", XDG_STATE_HOME=f"{r}/s", XDG_DATA_HOME=f"{r}/d",
                        XDG_CACHE_HOME=f"{r}/c", DISPLAY=display, SDL_AUDIODRIVER="dummy", LUXIUM_BIN=luxium,
                        LUX_NO_DIALOG="1", LUXIUM_HOST_GRACE="1")
        self.env.pop("WAYLAND_DISPLAY", None)
        if extra_env: self.env.update(extra_env)
        self.procs = []

    def log_path(self, app):
        return f"{self.root}/s/lux-desktop/{app}.log"

    def log(self, app):
        try: return open(self.log_path(app), errors="replace").read()
        except OSError: return ""

    def start(self, app_bin, args=(), env=None, tag="a"):
        e = dict(self.env, **(env or {}))
        p = subprocess.Popen([app_bin, *args], env=e, stdout=open(f"{self.root}/out-{tag}.txt", "w"), stderr=subprocess.STDOUT,
                             start_new_session=True)
        self.procs.append(p)
        return p

    def wait_log(self, app, text, timeout=30, proc=None):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.log(app): return True
            if proc and proc.poll() is not None: return text in self.log(app)
            time.sleep(0.1)
        return False

    def port(self, app):
        ms = re.findall(r"server up on port (\d+)", self.log(app))
        return int(ms[-1]) if ms else 0

    def host_hello(self, timeout=2.0):
        try:
            s = socket.socket(socket.AF_UNIX); s.settimeout(timeout); s.connect(f"{self.rt}/host.sock")
            s.sendall(b'{"cmd":"hello"}\n')
            return json.loads(s.makefile().readline())
        except (OSError, ValueError):
            return None

    def stray(self):
        """PIDs (other than ours) whose environment mentions this sandbox: leftovers of a run."""
        out = []
        for d in os.listdir("/proc"):
            if not d.isdigit() or int(d) == os.getpid(): continue
            try: env = open(f"/proc/{d}/environ", "rb").read()
            except OSError: continue
            if self.root.encode() in env: out.append(int(d))
        return out

    def cleanup(self):
        for p in self.stray():
            try: os.kill(p, signal.SIGKILL)
            except OSError: pass
        for p in self.procs:
            if p.poll() is None: p.kill()
        shutil.rmtree(self.root, ignore_errors=True)


def http(port, path, headers=None, method="GET", data=None, timeout=10):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers or {}, method=method, data=data)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r: return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e: return e.code, e.read(), dict(e.headers)
    except OSError as e: return 0, str(e).encode(), {}


def port_open(port):
    s = socket.socket(); s.settimeout(1)
    try: s.connect(("127.0.0.1", port)); return True
    except OSError: return False
    finally: s.close()


def wait_gone(pids, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        if not any(os.path.exists(f"/proc/{p}") and open(f"/proc/{p}/stat").read().split(")")[-1].split()[0] != "Z" for p in pids): return True
        time.sleep(0.1)
    return False


def descendants(pid):
    kids = {}
    for d in os.listdir("/proc"):
        if d.isdigit():
            try: kids.setdefault(int(open(f"/proc/{d}/stat").read().rsplit(")", 1)[1].split()[1]), []).append(int(d))
            except (OSError, IndexError): pass
    out, todo = [], [pid]
    while todo:
        for c in kids.get(todo.pop(), []): out.append(c); todo.append(c)
    return out


def scenario_luxium(app_id, app_bin, args, luxium, display, routes, setup=None, extra_env=None, name="luxium"):
    """Window through the Luxium host: server, window, routes, SIGTERM leaves nothing behind."""
    print(f"[{name}]")
    sb = Sandbox(display, luxium, extra_env)
    try:
        if setup: setup(sb)
        p = sb.start(app_bin, args)
        check(sb.wait_log(app_id, "window opened", 40, p), "window opened via the Luxium host")
        port = sb.port(app_id)
        check(port > 0 and port_open(port), f"server answers on loopback (port {port})")
        h = sb.host_hello()
        check(bool(h) and h.get("windows") == 1, f"host reports one window ({h and h.get('windows')})")
        for path, hdr, want in routes: route_check(port, path, hdr, want)
        kids = descendants(p.pid)
        p.send_signal(signal.SIGTERM)
        check(wait_gone([p.pid], 20), "SIGTERM: app exits")
        p.poll()
        check(p.returncode in (0, -15), f"exit status {p.returncode}")
        check(not port_open(port), "port closed")
        check(wait_gone(kids, 5), "no orphan children of the app")
        check(wait_gone(sb.stray(), 15), f"no leftover process in the sandbox (host exits after its grace) {sb.stray()}")
        check(not os.path.exists(f"{sb.rt}/host.sock"), "host socket removed")
    finally:
        sb.cleanup()


def route_check(port, path, hdr, want):
    st, body, h = http(port, path, hdr or {})
    ok = st == want[0] if isinstance(want, tuple) else st == want
    if ok and isinstance(want, tuple) and len(want) > 1 and want[1]: ok = want[1] in body.decode("utf8", "replace") or want[1] in str(h)
    check(ok, f"GET {path} {hdr or ''} -> {st}" + ("" if ok else f" ({body[:80]!r})"))


def scenario_webkit(app_id, app_bin, args, display, routes, setup=None, extra_env=None, no_binary=True):
    """No Luxium binary (or forced webkit backend): the GTK + WebKitGTK window must come up and shut down clean."""
    name = "webkit fallback, no luxium binary" if no_binary else "webkit forced"
    print(f"[{name}]")
    env = {"PATH": "/usr/local/bin:/usr/bin:/bin"} if no_binary else {"LUX_DESKTOP_BACKEND": "webkit"}
    if no_binary: env["LUXIUM_BIN"] = ""   # an empty variable counts as unset in find_binary()
    env.update(extra_env or {})
    sb = Sandbox(display, "", env)
    try:
        if setup: setup(sb)
        p = sb.start(app_bin, args)
        check(sb.wait_log(app_id, "server up on port", 40, p), "server up")
        port = sb.port(app_id)
        time.sleep(2)   # the webview needs a moment to map
        check(p.poll() is None, "app still running with its WebKitGTK window")
        check("using WebKitGTK" in sb.log(app_id) or not no_binary, "log says WebKitGTK is in use")
        check(not os.path.exists(f"{sb.rt}/host.sock"), "no Luxium host was started")
        for path, hdr, want in routes: route_check(port, path, {k: v for k, v in (hdr or {}).items() if k != "Accept"} if False else hdr, want)
        kids = descendants(p.pid)
        p.send_signal(signal.SIGTERM)
        check(wait_gone([p.pid], 20), "SIGTERM: app exits")
        p.poll()
        check(p.returncode in (0, -15), f"exit status {p.returncode}")
        check(not port_open(port), "port closed")
        check(wait_gone(kids, 5), "no orphan children (WebKit helper processes)")
        check(wait_gone(sb.stray(), 5), f"no leftover process in the sandbox {sb.stray()}")
    finally:
        sb.cleanup()
