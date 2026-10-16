"""Read an SFZ instrument and work out what each hit sounds like at the author's default settings.

Used only when building Barline's kits (tools/kits/build.py); nothing here ships in the program.
An SFZ drum kit plays several regions at once for one hit (one per microphone), each with its own
volume, pan, delay, start offset, and tuning, some switched by controller values. render_hit()
mixes all of that into one stereo sample, the way a sampler would play it.
"""

import math
import os
import re
import subprocess

import numpy as np

RATE = 44100   # the looper pedal runs at 44.1 kHz; everything is made at that rate

HEADERS = ("control", "global", "master", "group", "region", "curve", "effect", "midi", "sample")


def _strip_comments(text):
    out = []
    for line in text.split("\n"):
        i = line.find("//")
        out.append(line if i < 0 else line[:i])
    return "\n".join(out)


def _expand(path, defines, seen, root=None):
    """The file's text with #include pulled in and $DEFINES replaced (SFZ v2 preprocessor). Included
    paths are relative to the top file's folder (root), like samples."""
    if path in seen or len(seen) > 200:
        return ""
    seen = seen | {path}
    with open(path, encoding="utf-8", errors="replace") as f:
        text = _strip_comments(f.read())
    out = []
    base = root or os.path.dirname(path)
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#define"):   # one or more on a line
            for m in re.finditer(r'#define\s+(\$\w+)\s+(\S+)', s):
                defines[m.group(1)] = m.group(2).strip()
            continue
        if "#include" in line:   # possibly several on one line
            parts = []
            last = 0
            for m in re.finditer(r'#include\s+"([^"]+)"', line):
                parts.append(line[last:m.start()])
                inc = m.group(1)
                for k in sorted(defines, key=len, reverse=True):
                    inc = inc.replace(k, defines[k])
                target = os.path.normpath(os.path.join(base, inc))
                if not os.path.exists(target):   # some libraries include relative to the including file
                    target = os.path.normpath(os.path.join(os.path.dirname(path), inc))
                parts.append("\n" + _expand(target, defines, seen, base) + "\n")
                last = m.end()
            parts.append(line[last:])
            out.append("".join(parts))
            continue
        for k in sorted(defines, key=len, reverse=True):
            line = line.replace(k, defines[k])
        out.append(line)
    return "\n".join(out)


def _tokens(text):
    """(header) and (opcode, value) pairs in order. Values may contain spaces (sample paths)."""
    pos = 0
    toks = []
    pat = re.compile(r'<(\w+)>|(\w+)=')
    matches = list(pat.finditer(text))
    for i, m in enumerate(matches):
        if m.group(1):
            toks.append(("<", m.group(1)))
            continue
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        value = text[m.end():end].strip()
        if m.group(2) != "sample":   # other values never hold spaces: cut at the first one
            value = value.split()[0] if value.split() else ""
        toks.append((m.group(2), value))
    return toks


class Instrument:
    def __init__(self, path, cc=None):
        self.path = path
        self.dir = os.path.dirname(path)
        defines = {}
        text = _expand(path, defines, set())
        self.control = {}
        self.cc = {}
        self.regions = []
        levels = {"global": {}, "master": {}, "group": {}}
        current = None
        where = None
        for k, v in _tokens(text):
            if k == "<":
                where = v
                if v == "control":
                    current = self.control
                elif v in levels:
                    # a new level resets the ones below it
                    order = ["global", "master", "group"]
                    for lv in order[order.index(v):]:
                        levels[lv] = {}
                    current = levels[v]
                elif v == "region":
                    current = {}
                    merged = dict(levels["global"])
                    merged.update(levels["master"])
                    merged.update(levels["group"])
                    current.update(merged)
                    self.regions.append(current)
                else:
                    current = None
                continue
            if current is None:
                continue
            if where == "region":
                current[k] = v
            else:
                current[k] = v
        for k, v in self.control.items():
            m = re.match(r'set_(hd)?cc(\d+)$', k)
            if m:
                try:   # set_hdcc is 0..1; set_cc is 0..127
                    self.cc[int(m.group(2))] = float(v) * (127.0 if m.group(1) else 1.0)
                except ValueError:
                    pass
        if cc:
            self.cc.update(cc)
        self.default_path = self.control.get("default_path", "")

    def ccv(self, n):
        return self.cc.get(n, 0.0)

    def active(self, r, key, vel, seq, rand):
        """Does region r play for this key/velocity/round-robin step/random value?"""
        lo = int(_note(r.get("lokey", r.get("key", "0"))))
        hi = int(_note(r.get("hikey", r.get("key", "127"))))
        if not (lo <= key <= hi):
            return False
        if not (int(num(r, "lovel", 1)) <= vel <= int(num(r, "hivel", 127))):
            return False
        if r.get("trigger", "attack") not in ("attack", "first"):
            return False
        for k, v in r.items():
            m = re.match(r'(lo|hi)cc(\d+)$', k)
            if m:
                c = self.ccv(int(m.group(2)))
                if m.group(1) == "lo" and c < float(v):
                    return False
                if m.group(1) == "hi" and c > float(v):
                    return False
        sl = int(num(r, "seq_length", 1))
        sp = int(num(r, "seq_position", 1))
        if sl > 1 and ((seq % sl) + 1) != sp:
            return False
        if not (num(r, "lorand", 0) <= rand < num(r, "hirand", 1.0001)):
            return False
        if "sw_last" in r or "sw_down" in r:   # keyswitched articulations: only the default one
            dflt = self.control.get("sw_default", r.get("sw_default"))
            want = r.get("sw_last", r.get("sw_down"))
            if dflt is None or _note(want) != _note(dflt):
                return False
        return True

    def sample_path(self, r):
        p = (self.default_path + r["sample"]).replace("\\", "/")
        return os.path.normpath(os.path.join(self.dir, p))

    def mod(self, r, base, name):
        """An opcode plus its controller modulations (name_ccN / name_onccN), at the current CC values."""
        v = float(r.get(name, base))
        for k, x in r.items():
            m = re.match(name + r'_(?:on)?cc(\d+)$', k)
            if m:
                v += float(x) * self.ccv(int(m.group(1))) / 127.0
        return v

    def gain(self, r, vel):
        """Linear gain for region r played at vel."""
        db = self.mod(r, 0, "volume")
        amp = num(r, "amplitude", 100) / 100.0
        for k, x in r.items():   # amplitude_ccN scales by the controller (ARIA's meaning)
            m = re.match(r'amplitude_(?:on)?cc(\d+)$', k)
            if m:
                amp *= float(x) / 100.0 * self.ccv(int(m.group(1))) / 127.0
        # the velocity curve: explicit points, or amp_veltrack (default 100%: (vel/127)^2)
        pts = sorted((int(m.group(1)), float(v)) for k, v in r.items() for m in [re.match(r'amp_velcurve_(\d+)$', k)] if m)
        if pts:
            pts = [(0, 0.0)] + pts if pts[0][0] > 0 else pts
            if pts[-1][0] < 127:
                pts.append((127, 1.0))
            vc = np.interp(vel, [p[0] for p in pts], [p[1] for p in pts])
        else:
            track = num(r, "amp_veltrack", 100) / 100.0
            vc = 1 - track + track * (vel / 127.0) ** 2
        return amp * vc * 10 ** (db / 20.0)


def num(r, key, default):
    """A numeric opcode, or the default when it's missing, empty, or not a number."""
    try:
        return float(r.get(key, default))
    except (TypeError, ValueError):
        return float(default)


def _note(v):
    """A key as a number: 36, or a name like c2 / f#3 (SFZ: c4 = 60)."""
    v = str(v).strip().lower()
    if re.match(r'^-?\d+$', v):
        return int(v)
    m = re.match(r'^([a-g])(#|b)?(-?\d+)$', v)
    if not m:
        return 0
    n = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}[m.group(1)]
    n += {"#": 1, "b": -1}.get(m.group(2) or "", 0)
    return n + (int(m.group(3)) + 1) * 12


_cache = {}


def load(path, cents=0.0):
    """A sample file as float32 stereo at 44.1 kHz, retuned by `cents` (ffmpeg does the resampling)."""
    key = (path, round(cents, 3))
    if key in _cache:
        return _cache[key]
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate",
                            "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
    sr = int(probe.stdout.strip().split(",")[0])
    af = f"asetrate={sr * 2 ** (cents / 1200.0):.4f},aresample={RATE}:resampler=soxr:precision=28" if abs(cents) > 0.5 else \
        f"aresample={RATE}:resampler=soxr:precision=28"
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-af", af, "-ac", "2", "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    a = np.frombuffer(raw, dtype=np.float32).reshape(-1, 2).copy()
    _cache[key] = a   # cleared after each drum (clear_cache)
    return a


def clear_cache():
    _cache.clear()


def render_hit(inst, key, vel, seq, rand=0.5):
    """Everything that plays for one hit, mixed: float32 stereo at 44.1 kHz (None if nothing plays)."""
    parts = []
    for r in inst.regions:
        if "sample" not in r or r["sample"].startswith("*"):
            continue
        if not inst.active(r, key, vel, seq, rand):
            continue
        cents = inst.mod(r, 0, "tune") + 100 * inst.mod(r, 0, "transpose")
        pk = r.get("pitch_keycenter")
        if pk is not None and str(pk).lower() != "sample":
            track = num(r, "pitch_keytrack", 100)
            cents += (key - _note(pk)) * track
        a = load(inst.sample_path(r), cents)
        off = int(inst.mod(r, 0, "offset") * RATE / 44100.0)
        a = a[max(0, off):]
        delay = int(round(inst.mod(r, 0, "delay") * RATE))
        g = inst.gain(r, vel)
        pan = max(-100.0, min(100.0, inst.mod(r, 0, "pan"))) / 100.0
        # constant-power balance
        lg, rg = math.cos((pan + 1) * math.pi / 4) * math.sqrt(2), math.sin((pan + 1) * math.pi / 4) * math.sqrt(2)
        parts.append((delay, a * np.array([g * lg, g * rg], dtype=np.float32)))
    if not parts:
        return None
    n = max(d + len(a) for d, a in parts)
    out = np.zeros((n, 2), dtype=np.float32)
    for d, a in parts:
        out[d:d + len(a)] += a
    return out


def layers(inst, key):
    """The velocity ranges that sound different for a key: [(lo, hi)], and how many round-robins."""
    edges = set()
    seq = 1
    rand_cuts = set()
    for r in inst.regions:
        if "sample" not in r:
            continue
        lo = _note(r.get("lokey", r.get("key", "0")))
        hi = _note(r.get("hikey", r.get("key", "127")))
        if not (lo <= key <= hi):
            continue
        edges.add((int(num(r, "lovel", 1)), int(num(r, "hivel", 127))))
        seq = max(seq, int(num(r, "seq_length", 1)))
        for k in ("lorand", "hirand"):
            if k in r:
                rand_cuts.add(float(r[k]))
    # split 1..127 at every boundary any region has
    bounds = sorted({lo for lo, _ in edges} | {hi + 1 for _, hi in edges} | {1, 128})
    ranges = [(bounds[i], bounds[i + 1] - 1) for i in range(len(bounds) - 1) if bounds[i] <= 127]
    rands = sorted(c for c in rand_cuts if 0 < c < 1)
    rand_points = []
    cuts = [0.0] + rands + [1.0]
    for i in range(len(cuts) - 1):
        rand_points.append((cuts[i] + cuts[i + 1]) / 2)
    return ranges, seq, rand_points
