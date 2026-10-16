"""Build Barline's drum kits from the open-source sample libraries they come from.

    python3 -I tools/kits/build.py SOURCES OUT [kit ...]

SOURCES holds each library as downloaded (one folder per kit, see fetch.sh); OUT gets one folder
per kit: kit.txt plus 16-bit WAV samples at 44.1 kHz (light, loads instantly). Each hit is the author's own mix (every
microphone, at the instrument's default settings), cut where it fades out. Every kit maps its drums
onto General MIDI drum notes, so every pattern plays on every kit.
"""

import multiprocessing
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfz  # noqa: E402

# General MIDI drum notes Barline's patterns use
NAMES = {35: "Kick 2", 36: "Kick", 37: "Side Stick", 38: "Snare", 39: "Clap", 40: "Rimshot", 41: "Floor Tom 2",
         42: "Hi-Hat Closed", 43: "Floor Tom", 44: "Hi-Hat Pedal", 45: "Low Tom", 46: "Hi-Hat Open", 47: "Mid Tom",
         48: "High Tom", 49: "Crash", 50: "High Tom 2", 51: "Ride", 52: "China", 53: "Ride Bell", 54: "Tambourine",
         55: "Splash", 56: "Cowbell", 57: "Crash 2", 58: "Vibraslap", 59: "Ride 2", 60: "Bongo High", 61: "Bongo Low",
         62: "Conga Mute", 63: "Conga High", 64: "Conga Low", 69: "Cabasa", 70: "Shaker", 73: "Guiro Short",
         74: "Guiro Long", 75: "Claves", 76: "Woodblock High", 77: "Woodblock Low", 81: "Triangle", 82: "Shaker 2",
         83: "Sleigh Bells", 84: "Bell Tree"}

# Open hi-hats stop when a closed or pedal hat is played (one choke group per kit)
HAT_CHOKE = [42, 44, 46]

KITS = {}   # filled in by kits.py


def trim(a, floor_db=-66.0, fade_ms=40):
    """Cut the silence after the sound dies away, with a short fade so the cut never clicks."""
    level = np.abs(a).max(axis=1)
    thr = 10 ** (floor_db / 20.0)
    above = np.nonzero(level > thr)[0]
    if len(above) == 0:
        return a[:64]
    end = min(len(a), above[-1] + int(0.01 * sfz.RATE))
    a = a[:end].copy()
    f = min(len(a), int(fade_ms / 1000.0 * sfz.RATE))
    if f > 0:
        a[-f:] *= np.linspace(1.0, 0.0, f, dtype=np.float32)[:, None]
    return a


def pick(n, k):
    """k indexes spread evenly over range(n) (all of them if k >= n)."""
    if k >= n:
        return list(range(n))
    return sorted({round(i * (n - 1) / (k - 1)) for i in range(k)}) if k > 1 else [n - 1]


def gated_reverb(a, seed, size=1.6, hold=0.26, release=0.035, wet=1.0, bright=0.6, comp=True):
    """The 1980s snare: a big bright reverb, chopped off by a noise gate after `hold` seconds."""
    rng = np.random.default_rng(seed)
    n = int(size * sfz.RATE)
    t = np.arange(n) / sfz.RATE
    ir = np.zeros((n, 2), dtype=np.float32)
    for ch in range(2):   # a dense, decaying noise tail (a hall), a little different per side
        noise = rng.standard_normal(n).astype(np.float32)
        if bright < 1:   # gently darker later in the tail
            k = int(sfz.RATE / (8000 * bright + 1500))
            noise = np.convolve(noise, np.ones(max(1, k)) / max(1, k), mode="same").astype(np.float32)
        ir[:, ch] = noise * np.exp(-t * 6.9 / size)
        ir[: int(0.012 * sfz.RATE), ch] = 0   # a short pre-delay
    ir /= np.sqrt(np.sum(ir ** 2, axis=0, keepdims=True)) + 1e-9
    L = len(a) + n
    size_fft = 1 << int(np.ceil(np.log2(L)))
    wet_sig = np.zeros((L, 2), dtype=np.float32)
    for ch in range(2):
        A = np.fft.rfft(a[:, ch], size_fft)
        B = np.fft.rfft(ir[:, ch], size_fft)
        wet_sig[:, ch] = np.fft.irfft(A * B, size_fft)[:L]
    dry = np.zeros((L, 2), dtype=np.float32)
    dry[: len(a)] = a
    mix = dry + wet_sig * wet * (np.abs(a).max() / (np.abs(wet_sig).max() + 1e-9)) * 0.9
    # the gate: open while the hit rings, held for `hold`, then shut fast
    g = np.ones(L, dtype=np.float32)
    h0 = int(hold * sfz.RATE)
    r0 = int(release * sfz.RATE)
    if h0 + r0 < L:
        g[h0:h0 + r0] = np.linspace(1, 0, r0, dtype=np.float32)
        g[h0 + r0:] = 0
    out = mix * g[:, None]
    if comp:   # a little compression: lift the body of the hit
        peak = np.abs(out).max() + 1e-9
        out = np.tanh(out / peak * 1.6) * peak / np.tanh(1.6)
    return out[: h0 + r0 + 64]


def write_wav(path, a):
    """16-bit stereo WAV at 44.1 kHz (light and quick to load; plenty for a live room)."""
    rng = np.random.default_rng(len(a))
    dither = (rng.random(a.shape) - rng.random(a.shape)) / 32768.0   # TPDF, so quiet tails stay smooth
    pcm = np.clip(np.round((a + dither) * 32767.0), -32768, 32767).astype("<i2")
    data = pcm.tobytes()
    import struct
    head = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, sfz.RATE, sfz.RATE * 4, 4, 16)
    with open(path, "wb") as f:
        f.write(head + b"data" + struct.pack("<I", len(data)) + data)


def hat_setting(inst, key, ccn, want):
    """The controller value (0 or 127) that gives the open (rings longer) or closed hi-hat."""
    lengths = {}
    for v in (0, 127):
        inst.cc[ccn] = v
        a = sfz.render_hit(inst, key, 127, 0)
        lengths[v] = len(trim(a)) if a is not None else 0
    pick_v = max(lengths, key=lengths.get) if want == "open" else min(lengths, key=lengths.get)
    print(f"    hi-hat on key {key}: cc{ccn}={pick_v} is {want} ({lengths})", flush=True)
    return pick_v


def render_raw(job):
    """A drum from plain sample files: layers soft to loud, each a glob of round-robin files."""
    slug, spec, sources, note, globs = job
    import glob as g
    layer_list = []
    n = len(globs)
    for i, pat in enumerate(globs):
        files = sorted(g.glob(os.path.join(sources, pat)))
        files = [files[k] for k in pick(len(files), spec.get("rr", 3))]
        sounds = [sfz.load(f) for f in files]
        hi = round(127 * (i + 1) / n)
        layer_list.append([1 if not layer_list else layer_list[-1][1] + 1, hi, sounds])
    sfz.clear_cache()
    print(f"  {slug}: {NAMES.get(note, note)}: {len(layer_list)} layers x {max((len(s) for _, _, s in layer_list), default=0)}", flush=True)
    return [(note, lo, hi, sounds) for lo, hi, sounds in layer_list if sounds]


def render_piece(job):
    """One drum of a kit: its velocity layers, each with a few round-robin hits (runs in a worker)."""
    slug, spec, sources, note, src = job
    if "raw" in spec:
        return render_raw(job)
    prog, key = src[0], src[1]
    extra = dict(src[2]) if len(src) > 2 else {}
    cc = dict(spec.get("cc", {}))
    hat = {k: v for k, v in extra.items() if isinstance(v, str)}
    cc.update({k: v for k, v in extra.items() if not isinstance(v, str)})
    inst = sfz.Instrument(os.path.join(sources, prog), cc=cc)
    for ccn, want in hat.items():
        inst.cc[ccn] = hat_setting(inst, key, ccn, want)
    ranges, seq, rands = sfz.layers(inst, key)
    variants = [(s, r) for s in range(seq) for r in rands]
    # keep the softest and loudest layers and the ones between, and a few round-robins
    keep_layers = pick(len(ranges), spec.get("layers", 4))
    keep_rr = pick(len(variants), spec.get("rr", 3))
    layer_list = []
    for li in keep_layers:
        lo, hi = ranges[li]
        sounds = []
        for vi in keep_rr:
            s, r = variants[vi]
            a = sfz.render_hit(inst, key, hi, s, r)
            if a is not None and np.abs(a).max() > 1e-5:
                sounds.append(a)
        if sounds:
            layer_list.append([lo, hi, sounds])
    # the kept layers cover 1..127 between them
    for i, L in enumerate(layer_list):
        L[0] = 1 if i == 0 else layer_list[i - 1][1] + 1
    if layer_list:
        layer_list[-1][1] = 127
    sfz.clear_cache()
    print(f"  {slug}: {NAMES.get(note, note)}: {len(layer_list)} layers x {max((len(s) for _, _, s in layer_list), default=0)}", flush=True)
    return [(note, lo, hi, sounds) for lo, hi, sounds in layer_list]


def build_kit(slug, spec, sources, out):
    folder = os.path.join(out, slug)
    os.makedirs(folder, exist_ok=True)
    lines = [f"name {spec['name']}", f"by {spec['by']}", f"license {spec['license']}", f"url {spec['url']}",
             f"style {spec['style']}"]
    if spec.get("note"):
        lines.append(f"about {spec['note']}")
    pieces = spec["raw"] if "raw" in spec else spec["map"]
    jobs = [(slug, spec, sources, note, src) for note, src in pieces.items()]
    with multiprocessing.Pool(min(6, len(jobs))) as pool:
        done = pool.map(render_piece, jobs)
    hits = [h for piece in done for h in piece]   # (note, lo, hi, [arrays])
    fx = spec.get("gated", {})
    if fx:   # the gated-reverb treatment on the drums it names: note -> hold seconds
        hits = [(n, lo, hi, [gated_reverb(a, seed=n * 100 + i, hold=fx[n]) for i, a in enumerate(ss)] if n in fx else ss)
                for n, lo, hi, ss in hits]
    if not hits:
        raise SystemExit(f"{slug}: nothing rendered")
    # one level for the whole kit: the loudest hit peaks at -1 dBFS (the soft ones stay soft)
    peak = max(float(np.abs(a).max()) for _, _, _, ss in hits for a in ss)
    gain = 10 ** (-1 / 20.0) / peak
    # and how loud its snare is, so kits can be matched by ear (stored; the program evens them out)
    snare = [a for n, lo, hi, ss in hits if n == 38 and hi == 127 for a in ss] or [hits[-1][3][0]]
    rms = float(np.sqrt(np.mean(np.square(snare[0][: int(0.15 * sfz.RATE)] * gain))))
    lines.append(f"loudness {20 * np.log10(max(rms, 1e-9)):.2f}")
    for note in sorted({h[0] for h in hits}):
        lines.append(f"piece {note} {spec.get('names', {}).get(note, NAMES.get(note, 'Drum'))}")
    if all(n in pieces for n in (42, 46)):
        lines.append("choke " + " ".join(str(n) for n in HAT_CHOKE if n in pieces))
    for note, lo, hi, sounds in hits:
        files = []
        for i, a in enumerate(sounds):
            name = f"n{note}_v{hi:03d}_r{i + 1}.wav"
            write_wav(os.path.join(folder, name), trim(a * gain))
            files.append(name)
        lines.append(f"layer {note} {lo} {hi} " + " ".join(files))
    with open(os.path.join(folder, "kit.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    size = sum(os.path.getsize(os.path.join(folder, x)) for x in os.listdir(folder))
    print(f"{slug}: {len(hits)} layers, {size / 1e6:.1f} MB", flush=True)


def main():
    from kits import KITS as defs
    sources, out = sys.argv[1], sys.argv[2]
    which = sys.argv[3:] or list(defs)
    for slug in which:
        spec = defs[slug]
        build_kit(slug, spec, os.path.join(sources, spec["source"]), out)


if __name__ == "__main__":
    main()
