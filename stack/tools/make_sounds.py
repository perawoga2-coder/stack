#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Maker Hip
"""STACK's built-in sounds: app/sounds_data.h and app/SOUNDS.md from tools/sounds.json and the audio it names.

    tools/make_sounds.py tools/sounds.json app/sounds_data.h app/SOUNDS.md [--wav DIR]

Every source is audio only (no preset data, no code of another firmware). The audio is not in the repository:
sounds.json's "roots" names each group's folder under $STACK_SOURCES (default: sources/), "origin" says what the
group is and its licence.

Each sound is cut (a one-shot ends on a short fade; a held sound gets a loop of whole periods, or for noise a long
cross-fade, baked into its end), brought to the same loudness, and stored as IMA ADPCM (4 bits a sample, mono) or,
for a single cycle, as 16-bit PCM.

Noise (sounds.json "noise": WIND, RAIN) is rendered down to DC: under 40 Hz it held more than any other sound (a
kick's included), which the loudness's K-weighting (its high-pass at 38 Hz) does not count. Since 4.06 a noise is
first cut below NOISE_LOWCUT (a 4th-order high-pass: two Butterworth sections), then brought to the loudness as any.

Loudness: the ITU-R BS.1770 K-weighting, then the loudest 100 ms (mean square); every sound is scaled to TARGET_DB
there. A sound whose peak would then pass PEAK is turned down to it (the list printed says which, and by how much).
The ADPCM is the IMA standard's (IMA Digital Audio Focus and Technical Working Groups, "Recommended Practices for
Enhancing Digital Audio Compatibility in Multimedia Systems", 1992): its two tables and its algorithm, low nibble
first, from predictor 0 and step index 0.
"""
import json, math, os, subprocess, sys, wave
import numpy as np

TARGET_DB = -15.5       # K-weighted mean square of the loudest 100 ms, dB re full scale
PEAK = 0.89             # no sound's own peak passes -1 dBFS
CK = 512                # a decoder state is kept every CK samples (app.c SND_CK)
NOISE_LOWCUT = 150.0    # Hz: a noise's low-cut (4.06); every other sound is as it was
CATS = ["DRUMS", "BASS", "KEYS", "PAD", "LEAD", "FX"]

IMA_STEP = [  # the standard's 89 step sizes, ten a row
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_encode(x):
    """x: int16 samples -> (bytes, the decoded samples, the decoder's state before each sample [(pred, index)]).
    Written from the IMA standard: the sign, then the three magnitude bits against the step, its half and its quarter;
    the encoder keeps the decoder's own value, so the two never drift apart."""
    value, index = 0, 0
    packed = bytearray((len(x) + 1) // 2)
    heard = np.zeros(len(x), dtype=np.int16)
    before = []
    for pos, want in enumerate(x.tolist()):
        before.append((value, index))
        size = IMA_STEP[index]
        gap = want - value
        nibble = 8 if gap < 0 else 0
        gap = abs(gap)
        moved = size // 8
        for bit, part in ((4, size), (2, size // 2), (1, size // 4)):
            if gap >= part:
                nibble += bit
                gap -= part
                moved += part
        value = int(np.clip(value - moved if nibble >= 8 else value + moved, -32768, 32767))
        index = int(np.clip(index + IMA_INDEX[nibble % 8], 0, 88))
        heard[pos] = value
        packed[pos // 2] |= nibble << (4 * (pos % 2))
    return bytes(packed), heard, before


def decode(path, rate):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-ac", "1"] + (["-ar", str(rate)] if rate else []) + ["-"]
    return np.frombuffer(subprocess.run(cmd, check=True, capture_output=True).stdout, dtype="<i2").astype(np.float64) / 32768.0


def native_rate(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=sample_rate", "-of", "csv=p=0", path],
                         check=True, capture_output=True, text=True).stdout
    return int(out.strip().splitlines()[0])


def biquad(x, b, a):
    y = np.zeros(len(x))
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = (v / a[0] for v in b)
    a1, a2 = a[1] / a[0], a[2] / a[0]
    for i, v in enumerate(x.tolist()):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def k_weight(x, rate):
    """ITU-R BS.1770's two filters at this sample rate: the high shelf (+4 dB above ~1.7 kHz), then the high-pass (38 Hz)"""
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi * f0 / rate)
    Vh, Vb = 10.0 ** (G / 20.0), (10.0 ** (G / 20.0)) ** 0.4996667741545416
    a0 = 1.0 + K / Q + K * K
    x = biquad(x, [(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0],
               [1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0])
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / rate)
    a0 = 1.0 + K / Q + K * K
    return biquad(x, [1.0, -2.0, 1.0], [1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0])


def low_cut(x, rate, hz):
    """a 4th-order Butterworth high-pass at hz (two sections, Q 0.541 and 1.307: the RBJ cookbook's high-pass)"""
    for q in (0.5411961001461969, 1.3065629648763766):
        w = 2.0 * math.pi * hz / rate
        al, c = math.sin(w) / (2.0 * q), math.cos(w)
        x = biquad(x, [(1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0], [1.0 + al, -2.0 * c, 1.0 - al])
    return x


def loudness_db(x, rate, loop=None):
    """the loudest 100 ms of the K-weighted sound, dB (a held sound: its loop played round for half a second after)"""
    if loop:
        ls, le = loop
        x = np.concatenate([x[:le]] + [x[ls:le]] * (int(0.5 * rate / max(1, le - ls)) + 1))
    y = k_weight(x, rate) ** 2
    w = min(len(y), int(0.1 * rate))
    c = np.concatenate([[0.0], np.cumsum(y)])
    ms = (c[w:] - c[:-w]) / w
    return 10.0 * math.log10(max(float(ms.max()), 1e-12))


def cut_loop(x, rate, hz, start_s, len_s, fade):
    """a loop that begins at start_s and is about len_s long, cross-faded over fade samples: of whole periods of hz
    (the length whose two ends match best), or as given for noise (hz 0)"""
    a = int(start_s * rate)
    if hz:
        per = rate / hz
        best = None
        for k in range(max(1, int(len_s * hz * 0.7)), int(len_s * hz * 1.3) + 2):
            L = int(round(k * per))
            if a + L + 8 >= len(x):
                break
            w = min(1024, L)
            e = float(np.mean((x[a - w:a] - x[a + L - w:a + L]) ** 2))
            if best is None or e < best[0]:
                best = (e, L)
        L = best[1]
    else:
        L = min(int(len_s * rate), len(x) - a - 8)
    X = min(fade, a, L)
    y = x[:a + L].copy()
    if hz:
        w = np.linspace(0.0, 1.0, X, endpoint=False)
        y[a + L - X:] = x[a + L - X:a + L] * (1.0 - w) + x[a - X:a] * w
    else:                                  # noise: equal power
        w = np.linspace(0.0, 1.0, X, endpoint=False)
        y[a + L - X:] = x[a + L - X:a + L] * np.sqrt(1.0 - w) + x[a - X:a] * np.sqrt(w)
    return y, a, a + L


def write_wav(path, x, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(x.astype("<i2").tobytes())


def main(argv):
    src, out, md = argv[1], argv[2], argv[3]
    wavdir = argv[argv.index("--wav") + 1] if "--wav" in argv else None
    spec = json.load(open(src))
    lines = ["/* SPDX-License-Identifier: MPL-2.0",
             " * Copyright (C) 2026 Maker Hip */",
             "/* GENERATED by tools/make_sounds.py from tools/sounds.json: do not edit.",
             " * STACK's built-in sounds: IMA ADPCM (4 bits a sample, low nibble first) or 16-bit PCM, mono, each at the same",
             " * loudness. Audio only; every sound's source and licence is in app/SOUNDS.md. */", ""]
    rows, table, total, turned, cktotal = [], [], 0, [], 0
    counts = {c: 0 for c in CATS}
    order = sorted(range(len(spec["sounds"])), key=lambda i: (CATS.index(spec["sounds"][i]["cat"]), i))
    for n, i in enumerate(order):
        s = spec["sounds"][i]
        path = os.environ.get("STACK_SOURCES", "sources") + "/" + spec["roots"][s["from"]] + "/" + s["file"]
        rate = s.get("rate") or native_rate(path)
        x = decode(path, s.get("rate"))
        nz = np.nonzero(np.abs(x) > 0.004)[0]            # the silence before the sound goes
        x = x[max(0, int(nz[0]) - 8):] if len(nz) else x
        if s.get("skip"):
            x = x[int(s["skip"] * rate):]
        if s.get("noise"):
            x = low_cut(x, rate, NOISE_LOWCUT)
        ls = le = 0
        if s.get("cycle"):
            ls, le = 0, len(x)
        elif s.get("loop"):
            x, ls, le = cut_loop(x, rate, s.get("hz", 0) if not s.get("noise") else 0, s["loop"][0], s["loop"][1],
                                 int(s.get("fade", 0.03) * rate))
        else:
            x = x[:int(s["secs"] * rate)]
            f = min(len(x), int(s.get("tail", 0.03) * rate))
            x[len(x) - f:] *= np.linspace(1.0, 0.0, f) ** 2
        g = 10.0 ** ((TARGET_DB + s.get("trim_db", 0.0) - loudness_db(x, rate, (ls, le) if le else None)) / 20.0)
        pk = float(np.max(np.abs(x))) * g
        if pk > PEAK:
            turned.append((s["name"], 20.0 * math.log10(PEAK / pk)))
            g *= PEAK / pk
        xi = np.clip(np.round(x * g * 32767.0), -32768, 32767).astype(np.int16)
        name = f"SND_{n}"
        lp = li = 0
        ck = "0"
        if s.get("cycle"):
            lines.append(f"static const int16_t {name}[{len(xi)}] = {{{','.join(str(int(v)) for v in xi)}}};")
            size, dec, fmt = 2 * len(xi), xi, "SF_PCM"
        else:
            data, dec, states = ima_encode(xi)
            if le:
                lp, li = states[ls]
            lines.append(f"static const uint8_t {name}[{len(data)}] = {{{','.join(str(v) for v in data)}}};")
            # the decoder's state every CK samples (its value, low byte first, and its step index): the EDIT page
            # starts a sound anywhere, and plays it backwards, from the nearest one
            cks = []
            for at in range(0, len(xi), CK):
                cks += [states[at][0] & 255, (states[at][0] >> 8) & 255, states[at][1]]
            ck = f"CK_{n}"
            lines.append(f"static const uint8_t {ck}[{len(cks)}] = {{{','.join(str(v) for v in cks)}}};")
            cktotal += len(cks)
            size, fmt = len(data), "0"
        total += size
        counts[s["cat"]] += 1
        flags = "|".join([f for f in (fmt, "SF_LOOP" if le else "", "SF_DRUM" if s.get("drum") else "") if f and f != "0"]) or "0"
        table.append(f'    {{"{s["name"]}", {name}, {len(xi)}u, {ls}u, {le}u, {rate}.0f, {s.get("hz", 261.626):.3f}f, '
                     f'{flags}, CAT_{s["cat"]}, {lp}, {li}, {ck}}},')
        rows.append((s["cat"], s["name"], spec["origin"][s["from"]][0], spec["origin"][s["from"]][1], s["file"], rate,
                     len(xi) / rate, size))
        if wavdir:
            write_wav(f"{wavdir}/{n:02d}_{s['cat']}_{s['name'].replace(' ', '_')}.wav", dec, rate)
        seam = ""
        if le and not s.get("cycle"):      # the loop's joint against the sound's usual step from sample to sample
            d = dec.astype(np.float64)
            seam = f"  seam {abs(d[ls] - d[le - 1]) / max(1.0, float(np.sqrt(np.mean(np.diff(d[ls:le]) ** 2)))):.1f}"
        print(f'{s["cat"]:5s} {s["name"]:12s} {len(xi) / rate:5.2f} s {rate:5d} Hz {size / 1024:5.1f} KiB  peak {float(np.max(np.abs(dec))) / 32768:.2f}{seam}')
    lines += ["", "static const sound_t SOUNDS[] = {"] + table + ["};", f"#define NSOUNDS {len(table)}u", ""]
    open(out, "w").write("\n".join(lines))
    doc = ["# STACK: the built-in sounds", "",
           "Made by `tools/make_sounds.py` from `tools/sounds.json`. Every sound is audio only: no preset data and no code of",
           "another firmware is in STACK. From Felucca only rendered audio is used, with credit: Felucca by Leo Kuroshita",
           "(Hügelton Instruments).", "",
           f"{len(rows)} sounds, {total / 1024:.1f} KiB: " + ", ".join(f"{c} {counts[c]}" for c in CATS) + ".",
           "Stored as IMA ADPCM (4 bits a sample) or, the single cycles, 16-bit PCM; mono. Every sound is brought to the same",
           f"loudness (ITU-R BS.1770 K-weighting, the loudest 100 ms at {TARGET_DB:g} dB) with its own peak at {20 * math.log10(PEAK):.1f} dBFS at most.",
           f"The noises (WIND, RAIN) are first cut below {NOISE_LOWCUT:g} Hz (since 4.06): rendered down to DC, they held more",
           "under 40 Hz than any other sound, a rumble under the whole mix.",
           "", "| # | Category | Name | Source | Licence | File | Rate | Length |", "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for n, r in enumerate(rows):
        doc.append(f"| {n + 1} | {r[0]} | {r[1]} | {r[2]} | {r[3]} | `{r[4]}` | {r[5]} Hz | {r[6]:.2f} s |")
    doc += ["", "Sources:", ""] + [f"- **{k}**: {v[0]}. {v[1]}. {v[2]}" for k, v in spec["origin"].items()] + [""]
    open(md, "w").write("\n".join(doc))
    print(f"total {total / 1024:.1f} KiB ({total} B) in {len(rows)} sounds: " + ", ".join(f"{c} {counts[c]}" for c in CATS))
    print(f"  and {cktotal} B of decoder states, one every {CK} samples (the EDIT page)")
    for name, db in turned:
        print(f"  turned down to keep its peak: {name} {db:.1f} dB")


if __name__ == "__main__":
    main(sys.argv)
