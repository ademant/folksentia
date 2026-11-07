#!/usr/bin/env python3
"""Generate EBU Tech 3341 test signals for loudness and true peak.

The official EBU loudness test set cannot be downloaded by script (the EBU
site refuses non-browser requests). The cases checked here are fully defined
in EBU Tech 3341, so they are synthesised instead. If you download the official
zip by hand from https://tech.ebu.ch/publications/ebu_loudness_test_set and
unpack it into $FOLKSENTIA_EVAL_DATA/ebu/official/, check_ebu.py checks those
files too.

Writes 24-bit stereo WAV files plus expected.json to $FOLKSENTIA_EVAL_DATA/ebu/synthetic/.
Uses only the Python standard library.
"""

import json
import math
import struct
import wave

from common import data_dir

FS = 48000

# Each case: segments of (seconds, dBFS peak level), 1 kHz sine unless noted.
# expected: integrated LUFS (tolerance +-0.1 LU) and/or true peak dBTP
# (tolerance +0.2/-0.4 dB, Tech 3341 section 2.6).
LOUDNESS_CASES = {
    "3341-1": [(20, -23.0)],
    "3341-2": [(20, -33.0)],
    "3341-3": [(10, -36.0), (60, -23.0), (10, -36.0)],
    "3341-4": [(10, -72.0), (10, -36.0), (60, -23.0), (10, -36.0), (10, -72.0)],
    "3341-5": [(20, -26.0), (20.1, -20.0), (20, -26.0)],
}

# True-peak cases: sine at a fraction of fs with a start phase chosen so the
# samples miss the true peak; peak level -6 dBFS, so the true peak is -6 dBTP.
TRUE_PEAK_CASES = {
    "3341-15": (FS / 4, 0.0),
    "3341-16": (FS / 4, 45.0),
    "3341-17": (FS / 6, 60.0),
    "3341-18": (FS / 8, 67.5),
}


def db(x):
    return 10 ** (x / 20)


def write_wav(path, frames, channels=2, fs=FS):
    """frames: iterable of float samples (one per frame), duplicated per channel."""
    full = (1 << 23) - 1
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(3)
        w.setframerate(fs)
        buf = bytearray()
        for s in frames:
            v = max(-full - 1, min(full, round(s * full)))
            b = struct.pack("<i", v)[:3]
            buf += b * channels
            if len(buf) > 1 << 20:
                w.writeframes(bytes(buf))
                buf.clear()
        w.writeframes(bytes(buf))


def tone_segments(segments, freq=1000.0, fs=FS):
    """Continuous-phase sine whose level steps between segments."""
    n0 = 0
    for secs, level in segments:
        a = db(level)
        n = round(secs * fs)
        for i in range(n0, n0 + n):
            yield a * math.sin(2 * math.pi * freq * i / fs)
        n0 += n


def true_peak_tone(freq, phase_deg, level=-6.0, secs=5.0, fade=0.1):
    """Sine with raised-cosine fades so the start/end steps add no overshoot."""
    a = db(level)
    n = round(secs * FS)
    nf = round(fade * FS)
    ph = math.radians(phase_deg)
    for i in range(n):
        g = 1.0
        if i < nf:
            g = 0.5 - 0.5 * math.cos(math.pi * i / nf)
        elif i >= n - nf:
            g = 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / nf)
        yield g * a * math.sin(2 * math.pi * freq * i / FS + ph)


def main():
    out = data_dir() / "ebu" / "synthetic"
    out.mkdir(parents=True, exist_ok=True)
    expected = {}

    for name, segs in LOUDNESS_CASES.items():
        write_wav(out / f"{name}.wav", tone_segments(segs))
        expected[name] = {"integratedLufs": -23.0 if name != "3341-2" else -33.0,
                          "source": "EBU Tech 3341 test case, synthesised"}

    for name, (freq, phase) in TRUE_PEAK_CASES.items():
        write_wav(out / f"{name}.wav", true_peak_tone(freq, phase))
        expected[name] = {"truePeakDbtp": -6.0,
                          "source": "EBU Tech 3341 test case, synthesised"}

    # folksentia-specific checks of documented behaviour (README "loudness").
    write_wav(out / "fs-44100.wav",
              tone_segments([(20, -23.0)], fs=44100), fs=44100)
    expected["fs-44100"] = {"integratedLufs": -23.0,
                            "source": "case 1 at 44.1 kHz: measured at the file's own rate"}
    write_wav(out / "mono.wav", tone_segments([(20, -23.0)]), channels=1)
    expected["mono"] = {"integratedLufs": -23.0,
                        "source": "mono is measured as dual mono"}
    write_wav(out / "silence.wav", (0.0 for _ in range(10 * FS)))
    expected["silence"] = {"integratedLufs": None, "truePeakDbtp": None,
                           "source": "digital silence gives null"}

    (out / "expected.json").write_text(json.dumps(expected, indent=2) + "\n")
    print(f"wrote {len(expected)} signals to {out}")


if __name__ == "__main__":
    main()
