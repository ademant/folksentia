#!/usr/bin/env python3
"""Smoke test for folksentia: synthesise small WAV files, run the binary and
check the JSON. Standard library only.

  tests/smoke.py builddir/folksentia      (or: meson test -C builddir)
"""

import base64
import json
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import wave

BINARY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "builddir/folksentia"
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def write_wav(path, rate, channels, samples):
    """samples: list of floats in -1..1 (interleaved when stereo)."""
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, round(s * 32767))))
                               for s in samples))


def clicks(rate=44100, bpm=120.0, seconds=20.0, lead=1.0, tail=0.5):
    """Click track: a 1 kHz click on every beat, plus a 60 Hz bass hit on the
    first beat of each 4/4 bar. `lead`/`tail` seconds of digital silence."""
    n_lead, n_body, n_tail = int(lead * rate), int(seconds * rate), int(tail * rate)
    body = [0.0] * n_body
    period = 60.0 / bpm
    beat = 0
    while beat * period < seconds - 0.2:
        start = int(beat * period * rate)
        for k in range(int(0.03 * rate)):
            body[start + k] += 0.5 * math.sin(2 * math.pi * 1000 * k / rate) * math.exp(-k / (0.008 * rate))
        if beat % 4 == 0:
            for k in range(int(0.15 * rate)):
                body[start + k] += 0.4 * math.sin(2 * math.pi * 60 * k / rate) * math.exp(-k / (0.05 * rate))
        beat += 1
    return [0.0] * n_lead + body + [0.0] * n_tail


def run(*args):
    p = subprocess.run([BINARY, *args], capture_output=True)
    out = None
    if p.stdout:
        try:
            out = json.loads(p.stdout)
        except json.JSONDecodeError as e:
            check(False, f"valid JSON from {args}: {e}")
    return p.returncode, out, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def near(value, target, tol):
    return value is not None and abs(value - target) <= tol


def main():
    tmp = tempfile.mkdtemp(prefix="folksentia-test-")
    j = lambda name: os.path.join(tmp, name)

    mono = clicks()
    write_wav(j("clicks.wav"), 44100, 2, [s for s in mono for _ in range(2)])
    write_wav(j("silence.wav"), 48000, 1, [0.0] * 48000 * 3)
    rnd = random.Random(1)
    noisy = [rnd.uniform(-0.002, 0.002) for _ in range(2 * 44100)]  # about -59 dBFS
    noisy += [0.5 * math.sin(2 * math.pi * 440 * k / 44100) for k in range(44100)]
    write_wav(j("noisy.wav"), 44100, 1, noisy)
    with open(j("broken.wav"), "wb") as f:
        f.write(b"RIFF not really a wav file")

    # Click track: tempo, beats, downbeats, silence, schema.
    rc, out, text, _ = run("--downbeats", "--waveform=10", j("clicks.wav"))
    check(rc == 0, "clicks: exit 0")
    check(isinstance(out, dict) and out["schema"] == 1, "clicks: single object, schema 1")
    check(near(out["bpm"], 120, 2), f"clicks: bpm ~120 (got {out['bpm']})")
    check(near(out["duration"], 21.5, 0.01), f"clicks: duration 21.5 s (got {out['duration']})")
    check(near(out["silence"]["leadMs"], 1000, 10), f"clicks: leadMs ~1000 (got {out['silence']['leadMs']})")
    check(near(out["silence"]["tailMs"], 970, 20), f"clicks: tailMs ~970 (got {out['silence']['tailMs']})")
    check(out["silence"]["thresholdDb"] == -100, "clicks: default silence threshold -100")
    check(len(out["beats"]) >= 30, f"clicks: beats found ({len(out['beats'])})")
    beats_text = re.search(r'"beats": \[([^\]]*)\]', text).group(1)
    check(all(re.fullmatch(r"\d+\.\d{3}", b.strip()) for b in beats_text.split(",")),
          "clicks: beats printed with 3 decimals")
    db = out["downbeatIndexes"]
    check(db is not None and all(b - a == 4 for a, b in zip(db, db[1:])), f"clicks: downbeats every 4 beats ({db})")
    if db:
        # bass hits are at 1 s + multiples of 2 s
        phase = (out["beats"][db[0]] - 1.0) % 2.0
        check(min(phase, 2.0 - phase) < 0.07, f"clicks: downbeats on the bass hits (offset {phase:.3f} s)")
    check(out["waveform"]["bins"] == 10 and max(out["waveform"]["peaks"]) == 255, "clicks: waveform")
    check(out["loudness"]["integratedLufs"] is not None and out["loudness"]["truePeakDbtp"] is not None,
          "clicks: loudness measured")
    for field in ("key", "scale", "key_strength", "tuning_frequency", "tuning_cents", "tuning_spread_cents"):
        check(field in out, f"clicks: has {field}")
    check((out["key"] is None) == (out["key_strength"] < 0.6), "clicks: key null exactly when strength < 0.6")

    # Equal clicks: the onsets give the beat but no bar, so the bar is a whole
    # number of beats.
    rh = out["rhythm"]
    beats_per_bar = rh and rh["barSec"] / 0.5
    check(rh is not None and rh["schema"] == 1 and near(beats_per_bar, round(beats_per_bar), 0.05)
          and near(rh["barsPerMin"], 60 / rh["barSec"], 0.1),
          f"clicks: rhythm bar is a whole number of beats ({rh and (rh['barPulses'], rh['pulseSec'], rh['barSec'])})")
    check(rh is not None and rh["accents"]["energy"][0] == 1
          and len(rh["accents"]["energy"]) == rh["barPulses"] == len(rh["accents"]["bass"]),
          "clicks: one accent value per pulse, strongest first")
    check(rh is not None and len(rh["tempoCurve"]) == 1 and rh["tempoCurve"][0]["startSec"] == 0,
          "clicks: one tempo curve point for a file under 30 s")

    # Accented clicks in 3/4 at 150 BPM (loud first beat): bar of 3 beats, 1.2 s.
    rate, n = 44100, 44100 * 40
    waltz = [0.0] * n
    for b in range(int(40 / 0.4) - 1):
        start, amp = int(b * 0.4 * rate), (0.8 if b % 3 == 0 else 0.15)
        for k in range(int(0.03 * rate)):
            waltz[start + k] += amp * math.sin(2 * math.pi * 1000 * k / rate) * math.exp(-k / (0.008 * rate))
    write_wav(j("waltz.wav"), rate, 1, waltz)
    rc, w, _, _ = run(j("waltz.wav"))
    wr = w["rhythm"]
    check(wr is not None and near(wr["barSec"], 1.2, 0.05) and near(wr["barsPerMin"], 50, 2),
          f"accented 3/4 clicks: bar 1.2 s ({wr and (wr['barPulses'], wr['pulseSec'], wr['barSec'])})")
    e = wr and wr["accents"]["energy"]
    check(e is not None and e[0] == 1 and all(v < 0.6 for v in e[1:]),
          f"accented 3/4 clicks: one loud beat per bar in the accents ({e})")

    # Tempo must not depend on the analysis rate.
    rc, out, _, _ = run("-r", "16000", j("clicks.wav"))
    check(rc == 0 and near(out["bpm"], 120, 2), f"clicks -r 16000: bpm ~120 (got {out and out['bpm']})")

    rc, out, _, _ = run("--downbeats", "--meter", "3", j("clicks.wav"))
    check(rc == 0, "clicks --meter 3: exit 0")

    # Digital silence: unknown values are null.
    rc, out, _, _ = run("--beats", "--downbeats", j("silence.wav"))
    check(rc == 0, "silence: exit 0")
    for field in ("bpm", "bpm_confidence", "key", "scale", "key_strength", "tuning_frequency", "tuning_cents",
                  "tuning_spread_cents"):
        check(out[field] is None, f"silence: {field} is null")
    check(out["silence"]["leadMs"] is None and out["silence"]["tailMs"] is None, "silence: lead/tail null")
    check(out["loudness"]["integratedLufs"] is None and out["loudness"]["truePeakDbtp"] is None,
          "silence: loudness null")
    check(out["beats"] == [] and out["downbeatIndexes"] is None, "silence: no beats, no downbeats")
    check(out["rhythm"] is None, "silence: rhythm null")
    check(near(out["duration"], 3, 0.001), "silence: duration 3 s")

    # Silence threshold: noise counts as sound by default, as silence at -50 dB.
    rc, out, _, _ = run(j("noisy.wav"))
    check(rc == 0 and near(out["silence"]["leadMs"], 0, 10), f"noisy: leadMs ~0 by default (got {out['silence']['leadMs']})")
    rc, out, _, _ = run("--silence-threshold", "-50", j("noisy.wav"))
    check(rc == 0 and near(out["silence"]["leadMs"], 2000, 10),
          f"noisy -s -50: leadMs ~2000 (got {out['silence']['leadMs']})")
    check(out["silence"]["thresholdDb"] == -50, "noisy -s -50: thresholdDb reported")

    # Output shape.
    rc, out, _, _ = run("--array", j("silence.wav"))
    check(rc == 0 and isinstance(out, list) and len(out) == 1, "--array: array for one file")
    rc, out, _, _ = run(j("silence.wav"), j("silence.wav"))
    check(rc == 0 and isinstance(out, list) and len(out) == 2, "two files: array")

    # Errors.
    rc, out, _, _ = run(j("silence.wav"), j("broken.wav"))
    check(rc == 1 and isinstance(out, list) and "error" in out[1] and "bpm" in out[0],
          "broken file: exit 1, error object, other file still analysed")
    for args in (["-r", "abc"], ["-r", "0"], ["-r", "22050"], ["-r"], ["--meter", "1"], ["--meter", "3.5"],
                 ["--silence-threshold", "5"], ["--waveform=0"], ["--nope"], []):
        rc, _, _, _ = run(*args, *([j("silence.wav")] if args and args != ["-r"] else []))
        check(rc == 2, f"bad usage {args}: exit 2")

    # Non-UTF-8 file name.
    raw = os.path.join(os.fsencode(tmp), b"caf\xe9.wav")
    with open(j("silence.wav"), "rb") as src, open(raw, "wb") as dst:
        dst.write(src.read())
    p = subprocess.run([os.fsencode(BINARY), raw], capture_output=True)
    out = json.loads(p.stdout.decode("utf-8"))
    check(p.returncode == 0 and out["file"].endswith("caf�.wav")
          and base64.b64decode(out["file_base64"]) == raw, "non-UTF-8 name: file and file_base64")

    # Tuning: steady 440 Hz vs. a tone that rises by 47 cents halfway.
    def tones(parts, rate=22050):
        out = []
        for hz, secs in parts:
            out += [0.3 * (math.sin(2 * math.pi * hz * k / rate) + 0.5 * math.sin(4 * math.pi * hz * k / rate))
                    for k in range(int(secs * rate))]
        return out
    write_wav(j("steady.wav"), 22050, 1, tones([(440, 70)]))
    write_wav(j("drift.wav"), 22050, 1, tones([(440, 35), (440 * 2 ** (47 / 1200), 35)]))
    rc, steady, _, _ = run(j("steady.wav"))
    rc2, drift, _, _ = run(j("drift.wav"))
    check(near(steady["tuning_frequency"], 440, 1) and near(steady["tuning_spread_cents"], 0, 5),
          f"steady tone: 440 Hz, spread ~0 ({steady['tuning_frequency']}, {steady['tuning_spread_cents']})")
    check(drift["tuning_spread_cents"] is not None and drift["tuning_spread_cents"] > 30,
          f"tone rising 47 cents: large spread ({drift['tuning_spread_cents']})")

    # True peak is measured in blocks of 2^19 samples: a peak on a block
    # boundary must read the same as one inside a block.
    peaks = []
    for name, centre in (("tp-inside.wav", 200000), ("tp-boundary.wav", 1 << 19)):
        burst = [0.0] * (1 << 20)
        for k in range(centre - 2000, centre + 2000):
            burst[k] = 0.5 * math.sin(math.pi / 2 * k + math.pi / 4)  # fs/4 at 45°: inter-sample peaks
        write_wav(j(name), 48000, 1, burst)
        rc, out, _, _ = run(j(name))
        peaks.append(out["loudness"]["truePeakDbtp"])
    check(near(peaks[0], -6.0, 0.2) and near(peaks[1], peaks[0], 0.01),
          f"true peak across a block boundary ({peaks[1]}) equals inside ({peaks[0]})")

    # Version.
    p = subprocess.run([BINARY, "--version"], capture_output=True, text=True)
    lines = p.stdout.splitlines()
    check(p.returncode == 0 and len(lines) >= 3 and re.fullmatch(r"folksentia \d+\.\d+\.\d+(\+g[0-9a-f]+(\.dirty)?)?", lines[0])
          and re.fullmatch(r"built (\d{4}-\d\d-\d\d|unknown)", lines[1]) and lines[2].startswith("Essentia "),
          f"--version: version, build date, Essentia ({lines[:2]})")

    # Fingerprint, when Essentia has Chromaprint.
    rc, out, _, err = run("--fingerprint", j("clicks.wav"), j("silence.wav"))
    if rc == 2 and "Chromaprint" in err:
        print("skip fingerprint: Essentia built without Chromaprint")
    else:
        fp = out[0]["fingerprint"]
        check(rc == 0 and fp is not None and fp["algorithm"] == "chromaprint" and fp["version"] == 2,
              "fingerprint: present for clicks")
        check(out[1]["fingerprint"] is None, "fingerprint: null for silence")

    print(f"\n{len(failures)} failure(s)" if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
