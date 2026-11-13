#!/usr/bin/env python3
"""The online review on Linux (and anything POSIX): the counterpart of tools/online-review.ps1.

Starts the local lobby (server/eck-lobby/local), a TCP proxy in front of it that can cut every lobby connection on demand,
and N game instances with the test channel on; walks them into one session through the test channel, races, presses the
item button, cuts the lobby for a few seconds mid-race (reconnection), and at the end kills the last guest (a player who
vanishes). Writes review-N.txt, the logs, and screenshots when the `mss` module and an X display are there.

  ECK_GAME=/path/to/game python3 tools/online-review.py 2

Environment: ECK_EXE (default out/build/linux-amd64-release/el_chavo_kart), ECK_GAME (required), ECK_REVIEW (output folder,
default out/online-review), ECK_LOBBY_PORT (8791), ECK_SAMPLES (60), ECK_EVERY (2 s), ECK_ITEM_EVERY (samples between item
presses, 0 off), ECK_ITEM_HOLD (2.5 s: a press must outlast a frame, and a software renderer is slow), ECK_CUT_AT (sample of
the lobby cut, -1 off), ECK_CUT_SECS (8), ECK_KILL_GUEST (1), ECK_DISPLAYS (comma list, one per instance, e.g. :77,:78;
default the current DISPLAY), ECK_FORCE_RELAY=1 is passed through to the game (relay only).
"""
import glob, hashlib, json, os, re, signal, socket, subprocess, sys, threading, time

ROOT = os.environ.get("ECK_ROOT", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.environ.get("ECK_EXE", os.path.join(ROOT, "out", "build", "linux-amd64-release", "el_chavo_kart"))
GAME = os.environ["ECK_GAME"]
BASE = os.environ.get("ECK_REVIEW", os.path.join(ROOT, "out", "online-review"))
LOBBY_PORT = int(os.environ.get("ECK_LOBBY_PORT", "8791"))
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2
SAMPLES = int(os.environ.get("ECK_SAMPLES", "60"))
EVERY = float(os.environ.get("ECK_EVERY", "2"))
PROXY = os.environ.get("ECK_PROXY", "1") == "1"
PROXY_PORT = LOBBY_PORT + 2
LOBBY = f"ws://127.0.0.1:{PROXY_PORT if PROXY else LOBBY_PORT}/"
CUT_AT = int(os.environ.get("ECK_CUT_AT", "-1")); CUT_SECS = float(os.environ.get("ECK_CUT_SECS", "8"))
ITEM_EVERY = int(os.environ.get("ECK_ITEM_EVERY", "3")); KILL_GUEST = os.environ.get("ECK_KILL_GUEST", "1") == "1"
procs = []
report = os.path.join(BASE, f"review-{N}.txt")
os.makedirs(BASE, exist_ok=True)
rep = open(report, "w")

def note(t):
    line = time.strftime("%H:%M:%S ") + t
    print(line, flush=True); rep.write(line + "\n"); rep.flush()

def C(port, req, timeout=15.0):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=3.0) as s:
            s.settimeout(timeout); s.sendall((req + "\n").encode())
            buf, lines = b"", []
            while True:
                ch = s.recv(4096)
                if not ch: break
                buf += ch
                while b"\n" in buf:
                    l, buf = buf.split(b"\n", 1)
                    l = l.decode(errors="replace").strip()
                    if l: lines.append(l)
                    if l == "end": return "\n".join(lines)
            return "\n".join(lines)
    except OSError as e:
        return f"cannot reach 127.0.0.1:{port}: {e}"

TS = float(os.environ.get("ECK_TAPSCALE", "1"))
DISPLAYS = [d for d in os.environ.get("ECK_DISPLAYS", "").split(",") if d]
def display_of(i): return DISPLAYS[i % len(DISPLAYS)] if DISPLAYS else os.environ.get("DISPLAY", ":0")
try:
    import mss, mss.tools
except Exception:
    mss = None
def shot(tag):
    if mss is None: return
    for i in range(N):
        try:
            with mss.MSS(display=display_of(i)) as s:
                img = s.grab(s.monitors[0]); mss.tools.to_png(img.rgb, img.size, output=os.path.join(BASE, f"shot-{tag}-{chr(97+i)}.png"))
        except Exception: pass

class LobbyCut:
    """A TCP proxy for the lobby websocket that drops every connection while `cut` is set and refuses new ones."""
    def __init__(self, listen, target):
        self.cut, self.links, self.lock, self.target = False, [], threading.Lock(), target
        self.server = socket.socket(); self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", listen)); self.server.listen(16)
        threading.Thread(target=self.accept, daemon=True).start()
    def accept(self):
        while True:
            c, _ = self.server.accept()
            if self.cut: c.close(); continue
            try: u = socket.create_connection(("127.0.0.1", self.target))
            except OSError: c.close(); continue
            with self.lock: self.links += [c, u]
            for a, b in ((c, u), (u, c)): threading.Thread(target=self.pump, args=(a, b), daemon=True).start()
    def pump(self, a, b):
        try:
            while True:
                d = a.recv(65536)
                if not d: break
                b.sendall(d)
        except OSError: pass
        for x in (a, b):
            try: x.shutdown(socket.SHUT_RDWR)
            except OSError: pass
    def set_cut(self, cut):
        self.cut = cut
        if cut:
            with self.lock:
                for x in self.links:
                    try: x.shutdown(socket.SHUT_RDWR); x.close()
                    except OSError: pass
                self.links.clear()
def tap(port, keys, hold=400, gap=700):
    hold, gap = hold * TS, gap * TS
    for k in keys.split(","):
        C(port, f"pad {k} 0 0 0 0"); time.sleep(hold / 1000)
        C(port, "pad 0 0 0 0 0"); time.sleep(gap / 1000)

def logpath(p):
    files = sorted(glob.glob(os.path.join(BASE, p, "logs", "*.log")), key=os.path.getmtime)
    return files[-1] if files else None

def logtext(p):
    path = logpath(p)
    if not path: return []
    with open(path, encoding="utf-8", errors="replace") as f: return f.read().splitlines()

def wait_for(p, pattern, seconds):
    end = time.time() + seconds; rx = re.compile(pattern)
    while time.time() < end:
        hits = [l for l in logtext(p) if rx.search(l)]
        if hits: return hits[-1]
        time.sleep(0.7)
    return ""

def own(port):
    for l in C(port, "state").split("\n"):
        if l.startswith('{"race_state'):
            try: return json.loads(l)
            except Exception: return None
    return None

def alive(): return sum(1 for p in procs[1:] if p.poll() is None)

def stop_all():
    for p in procs:
        if p.poll() is None:
            try: os.killpg(p.pid, signal.SIGTERM)
            except Exception: pass
    time.sleep(2)
    for p in procs:
        if p.poll() is None:
            try: os.killpg(p.pid, signal.SIGKILL)
            except Exception: pass

players = [dict(name=chr(0x61 + i), udp=7101 + i, ctrl=18799 + i) for i in range(N)]
try:
    note("jogadores: " + " ".join(f"({p['name']} udp={p['udp']} ctrl={p['ctrl']})" for p in players))
    lob = open(os.path.join(BASE, "lobby.log"), "w")
    procs.append(subprocess.Popen(["node", "server/eck-lobby/local/server.js", "--host", "127.0.0.1", "--port", str(LOBBY_PORT), "--db", ":memory:"],
                                  cwd=ROOT, stdout=lob, stderr=subprocess.STDOUT, start_new_session=True))
    time.sleep(3)
    note("lobby ok" if procs[0].poll() is None else "lobby FAILED")
    proxy = LobbyCut(PROXY_PORT, LOBBY_PORT) if PROXY else None
    if proxy: note(f"lobby proxy {PROXY_PORT}->{LOBBY_PORT}")
    for i, p in enumerate(players):
        user = os.path.join(BASE, p["name"], "user_data"); logs = os.path.join(BASE, p["name"], "logs")
        os.makedirs(user, exist_ok=True); os.makedirs(logs, exist_ok=True)
        p["fresh"] = not os.path.exists(os.path.join(user, ".firstrun_done"))
        sp = os.path.join(user, "settings.toml")
        text = open(sp).read() if os.path.exists(sp) else ""
        kvs = [("eck_launcher", "false"), ("eck_skip_intro", "true"), ("eck_online_enabled", "true"),
               ("eck_online_connection_test", '"udp 7101-7110"'), ("eck_launcher_fullscreen", "false"),
               ("eck_launcher_maximized", "false"), ("eck_launcher_width", os.environ.get("ECK_W", "640")),
               ("eck_launcher_height", os.environ.get("ECK_H", "360")), ("eck_launcher_x", str(i * 660)),
               ("eck_launcher_y", "40"), ("detailed_logging", "true"), ("eck_internal_render_scale", "50"), ("window_width", "640"), ("window_height", "360"), ("eck_online_name", f'"Test-{p["name"]}"')]
        for k, v in kvs:
            if re.search(rf"(?m)^{k}\s*=", text): text = re.sub(rf"(?m)^{k}\s*=.*$", f"{k} = {v}", text)
            else: text += f"\n{k} = {v}\n"
        open(sp, "w").write(text)
        n = len(glob.glob(os.path.join(logs, "*.log"))) + 1
        env = dict(os.environ, DISPLAY=display_of(i), ECK_ONLINE_PORT=str(p["udp"]), ECK_INSTANCE=p["name"], ECK_LOBBY=LOBBY,
                   ECK_TEST_CONTROL="1", ECK_TEST_CONTROL_PORT=str(p["ctrl"]))
        out = open(os.path.join(logs, f"stdout_{n:03}.txt"), "w")
        procs.append(subprocess.Popen([EXE, f"--game_data_root={GAME}", f"--user_data_root={user}",
                                       f"--log_file={os.path.join(logs, f'el_chavo_kart_{n:03}.log')}"],
                                      cwd=os.path.dirname(EXE), env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True))
        note(f"abrindo {p['name']} pid={procs[-1].pid}")
        time.sleep(3)
    boot = float(os.environ.get("ECK_BOOT", "40"))
    t0 = time.time()
    while time.time() - t0 < boot:
        if all("end" in C(p["ctrl"], "help", 3) for p in players): break
        time.sleep(2)
    note(f"vivas apos boot: {alive()} de {N} ({time.time()-t0:.0f}s)")
    for p in players:
        h = C(p["ctrl"], "help")
        if "end" not in h: raise RuntimeError(f"canal {p['name']} morto: {h}")
        note(f"canal {p['name']}:{p['ctrl']} ok")
    time.sleep(float(os.environ.get("ECK_SETTLE", "15")))
    codes = {}
    for p in players:
        path = os.path.join(BASE, p["name"], "user_data", "eck_online_identity.ini")
        if os.path.isfile(path):
            m = re.search(r"token=([0-9a-f]{64})", open(path, errors="replace").read())
            if m:
                d = hashlib.sha256(("eck-player-id:" + m.group(1)).encode()).hexdigest()
                a = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
                c = "".join(a[int(d[2*i:2*i+2], 16) % 32] for i in range(8)); codes[p["name"]] = c[:4] + "-" + c[4:]
    note("codes: " + json.dumps(codes))
    for f in players:
        for t in players:
            if f is t: continue
            note(f"friend {f['name']}->{t['name']}: " + C(f["ctrl"], "friend add " + codes.get(t["name"], "?")).split("\n")[0])
    time.sleep(2)
    for p in players:
        note(f"menu {p['name']}: Start, (B no primeiro uso), desce ate Free Race, A fresh={p.get('fresh')}")
        tap(p["ctrl"], "16", 1300, 5000)
        if p.get("fresh"):
            tap(p["ctrl"], "8192", 600, 3000)
            open(os.path.join(BASE, p["name"], "user_data", ".firstrun_done"), "w").close()
        tap(p["ctrl"], "2,2,4096", 450, 800)
        tap(p["ctrl"], "4096", 450, 2400); tap(p["ctrl"], "4096", 450, 3200)
        shot(f"menu-{p['name']}")
    for i in range(1, N):
        g = players[i]
        inv = C(players[0]["ctrl"], "match host " + codes.get(g["name"], "?"))
        heard = wait_for(g["name"], "invite received", 20)
        acc = C(g["ctrl"], "match accept")
        joined = wait_for(g["name"], "session joined", 20)
        note(f"invite {g['name']}: {inv.splitlines()[0] if inv else ''} | heard={'yes' if heard else 'no'} | accept={acc.splitlines()[0] if acc else ''} | joined={'yes' if joined else 'no'}")
    for i, p in enumerate(players):
        if i > 0: tap(p["ctrl"], ",".join(["8"] * i), 300, 260)
        tap(p["ctrl"], "4096", 450, 900)
        note(f"pick {p['name']} seat={i}: " + C(p["ctrl"], f"pick 2 {i}").split("\n")[0])
    seat_lines = []
    for s in range(N):
        hit = wait_for("a", rf"panel{s} seat={s} .*state=2", 45)
        if not hit:
            note(f"pick retry seat={s}: " + C(players[s]["ctrl"], f"pick 2 {s}").split("\n")[0])
            hit = wait_for("a", rf"panel{s} seat={s} .*state=2", 20)
        seat_lines.append(f"panel{s}={'ok' if hit else 'PENDING'}")
    note("picks: " + " ".join(seat_lines)); shot("picks")
    L = players[0]
    note("host confirma, Start, 4x baixo ate Chavoltepec, A")
    tap(L["ctrl"], "4096", 700, 3000); tap(L["ctrl"], "16", 1500, 4000)
    tap(L["ctrl"], "2,2,2,2", 400, 500); tap(L["ctrl"], "4096", 700, 3500)
    shot("track"); pub = wait_for("a", "race published", 90)
    note("publish: " + ("ok  " + pub if pub else "FAILED"))
    for p in players:
        b = wait_for(p["name"], "race_begin", 120)
        note(f"race_begin {p['name']}: {b or 'MISSING'}")
    if pub:
        for p in players: C(p["ctrl"], "pad 0 0 0 255 0")
        note("throttle em todos; amostrando a corrida")
        last, lastlap, back, spread1, mism = {}, {}, {}, -1, 0
        views, items_seen, item_presses, item_mismatch, lap_mismatch = {}, set(), {}, 0, 0
        for s in range(SAMPLES):
            time.sleep(EVERY)
            if alive() != N: note(f"abortado: {alive()} vivas"); break
            if ITEM_EVERY > 0 and s % ITEM_EVERY == 0:
                shot(f"item-pre-{s}")
                for i, p in enumerate(players):
                    note(f"X press {p['name']} sample={s}")
                    C(p["ctrl"], "pad 16384 0 0 255 0"); time.sleep(float(os.environ.get("ECK_ITEM_HOLD", "2.5"))); C(p["ctrl"], "pad 0 0 0 255 0")
                    item_presses[p["name"]] = item_presses.get(p["name"], 0) + 1
                time.sleep(1.5); shot(f"item-post-{s}")
            if s == CUT_AT:
                if proxy:
                    note(f"corte do lobby por {CUT_SECS}s (todos os websockets)"); proxy.set_cut(True)
                    time.sleep(CUT_SECS); proxy.set_cut(False); note("lobby restaurado")
            values, states, parts = [], [], []
            for i, p in enumerate(players):
                j = own(p["ctrl"])
                if not j: parts.append(f"{p['name']}=no_state"); continue
                states.append(int(j["race_state"]))
                view = [f"s{k['slot']}:lap={k['lap']}:dist={k['distance']}:place={k['place']}" for k in j.get("karts", [])]
                mine = next((k for k in j.get("karts", []) if k["slot"] == i), None)
                views.setdefault(p["name"], {})
                for k in j.get("karts", []):
                    if k["slot"] < N: views[p["name"]][k["slot"]] = (k.get("item", 0), k["lap"], k.get("finished", None))
                    if k["slot"] < N and k.get("item", 0): items_seen.add((p["name"], k["slot"], k.get("item")))
                if mine:
                    d, lap = int(mine["distance"]), int(mine["lap"])
                    if p["name"] in last and d < last[p["name"]] - 500 and lap <= lastlap[p["name"]]:
                        back[p["name"]] = back.get(p["name"], 0) + 1
                    last[p["name"]], lastlap[p["name"]] = d, lap; values.append(d)
                parts.append(f"{p['name']} rs={j['race_state']} [{'; '.join(view)}]")
            spread = max(values) - min(values) if len(values) == N else -1
            if s == 1: spread1 = spread
            if s % 10 == 0: shot(f"race-{s}")
            if len(views) == N:
                for slot in range(N):
                    vals = [views[p["name"]].get(slot) for p in players]
                    if None not in vals and len(set(v[0] for v in vals)) > 1: item_mismatch += 1
                    if None not in vals and len(set(v[1] for v in vals)) > 1: lap_mismatch += 1
            note(f"sample={s} spread={spread}  " + "  |  ".join(parts))
            if len(states) == N and len(set(states)) > 1: mism += 1; note(f"RS DESSINC sample={s} rs={states}")
            if len(states) == N and all(x > 2 for x in states): note("corrida saiu do estado de pista (rs>2)"); break
        note(f"rs_mismatch_samples={mism}")
        note(f"item_presses={item_presses} items_seen(view,slot,item)={sorted(items_seen)} item_view_mismatch_samples={item_mismatch} lap_view_mismatch_samples={lap_mismatch}")
        shot("end")
        if KILL_GUEST and N > 1:
            g = players[-1]; gp = procs[-1]
            note(f"MATANDO convidado {g['name']} (SIGKILL) com a corrida em andamento")
            try: os.killpg(gp.pid, signal.SIGKILL)
            except Exception: pass
            # The lobby holds a dropped seat for its grace (30 s) before the host hears the guest left.
            quiet_at = 0
            for t in range(15):
                time.sleep(3)
                j = own(players[0]["ctrl"])
                note(f"host apos kill t={3*(t+1)}s: " + (f"rs={j['race_state']} in_race={j['in_race']} session={j.get('session')}" if j else "no_state"))
                host_log = logtext(players[0]["name"])
                if quiet_at and 3 * t == quiet_at: shot("guest-quiet-later")  # a slow renderer may not have drawn it yet
                if not quiet_at and any("guest_quiet seat=" in l for l in host_log):
                    quiet_at = 3 * (t + 1); note(f"host viu o convidado calado (guest_quiet) t<={quiet_at}s"); shot("guest-quiet")
                if any("guest_left seat=" in l for l in host_log): note(f"host soube da saida (guest_left) t={3*(t+1)}s"); break
            else: note("host NAO recebeu guest_left em 45s")
            note("host vivo apos kill do convidado: " + ("sim" if procs[1].poll() is None else "NAO"))
        note(f"start spread (t=1): {spread1}   backwards: {sum(back.values())}")
        for i, p in enumerate(players):
            j = own(p["ctrl"]); mine = j and next((k for k in j.get("karts", []) if k["slot"] == i), None)
            note(f"final {p['name']}: " + (json.dumps(j)[:400] if j else "no_state"))
except Exception as e:
    note(f"ERRO: {e!r}")
finally:
    for p in players:
        log = logtext(p["name"])
        errs = [l for l in log if "[error]" in l]; warns = [l for l in log if "[warning]" in l]
        marks = []
        for pat in ["session ended", "results before the host", "guest results", "world pawns", "finish_rx", "post_race",
                    "url.map=", "race travel abandoned", "guest holding", "rejected=[1-9]", "dropped_old=[1-9]",
                    "own_snaps=[1-9]", "remote jump smoothed", "remote teleport", "finish refused", "finish marked",
                    "race published", "invite received", "session joined", "race_begin", "start instant", "datagram refused",
                    "reconnected", "online resumed", "resume refused", "Connection lost", "transport=relay", "transport=direct", "relay failed",
                    "guest results", "seat left", "peer gone", "kReconnecting|reconnecting"]:
            c = sum(1 for l in log if re.search(pat, l))
            if c: marks.append(f"{pat}={c}")
        note(f"log {p['name']}: errors={len(errs)} warnings={len(warns)} " + " ".join(marks))
        for pat in ["race_begin", "start instant", "finish_rx", "finish refused", "finish marked", "remote jump smoothed",
                    "remote teleport", "ONLINE-NET\\] gameplay", "traffic", "reconnected", "online resumed", "resume refused",
                    "relay", "session ended", "guest results"]:
            for l in [l for l in log if re.search(pat, l)][:6]: note(f"  HIT {p['name']}: {l}")
    note("encerrando instancias e lobby"); stop_all()
    note(f"restantes={alive()}")
