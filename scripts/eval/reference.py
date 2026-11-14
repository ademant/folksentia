#!/usr/bin/env python3
"""Run independent reference tools on audio files, decoding each file once.

ffmpeg decodes the file a single time. Inside ffmpeg an `asplit` filter feeds
one branch to its own analysers (ebur128, silencedetect, astats) and every
other branch to a separate output in the format one reference tool needs. Each
output is written to a pipe (`pipe:N`) whose read end is that tool's stdin, so
all tools run in parallel on the same decoded audio. A tool that must seek in
its input (soundstretch) gets a temporary file instead and runs afterwards.

Prints one JSON object per file, with field names matching folksentia where
there is a counterpart. Tools that are not installed are skipped and listed
under "skipped". See REFERENCE_TOOLS.md for what each value is comparable to.

Usage:
    python3 reference.py tune.opus [more files...] [--silence-threshold -100]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Callable

RATE = 44100


@dataclass
class Tool:
    name: str
    binary: str
    argv: list[str]                  # reads the stream from stdin
    out_args: list[str]              # ffmpeg output options for this tool's stream
    parse: Callable[[str], dict]
    # Needs a seekable input: ffmpeg writes a temporary file instead of a
    # pipe, the tool gets its path in place of "{file}" and runs afterwards.
    seekable: bool = False


def parse_bpm_tools(out: str) -> dict:
    return {"bpm": float(out.split()[0])}


def parse_fpcalc(out: str) -> dict:
    d = json.loads(out)
    return {"fingerprint": d["fingerprint"]}


def parse_aubio_tempo(out: str) -> dict:
    m = re.search(r"([\d.]+)\s*bpm", out)
    return {"bpm": float(m.group(1))} if m else {}


def parse_aubio_beats(out: str) -> dict:
    return {"beats": [round(float(x.split()[0]), 3) for x in out.split("\n") if x.strip()]}


def parse_soundstretch(out: str) -> dict:
    m = re.search(r"Detected BPM rate\s*([\d.]+)", out)
    if m:
        return {"bpm": float(m.group(1))}
    if "Couldn't detect BPM" in out:
        return {"bpm": None}  # an answer: no steady tempo found
    return {}


MONO_F32 = ["-ac", "1", "-ar", str(RATE), "-f", "f32le"]
MONO_WAV = ["-ac", "1", "-ar", str(RATE), "-c:a", "pcm_s16le", "-f", "wav"]

TOOLS = [
    Tool("bpm-tools", "bpm", ["bpm"], MONO_F32, parse_bpm_tools),
    # folksentia fingerprints the first 120 s; cap the stream so fpcalc
    # reads it to the end instead of closing the pipe early.
    Tool("fpcalc", "fpcalc",
         ["fpcalc", "-format", "s16le", "-rate", str(RATE), "-channels", "1",
          "-length", "120", "-json", "-"],
         ["-t", "120", "-ac", "1", "-ar", str(RATE), "-f", "s16le"], parse_fpcalc),
    Tool("aubio-tempo", "aubio", ["aubio", "tempo", "/dev/stdin"], MONO_WAV, parse_aubio_tempo),
    Tool("aubio-beat", "aubio", ["aubio", "beat", "/dev/stdin"], MONO_WAV, parse_aubio_beats),
    # soundstretch rewinds its input after the BPM pass (WavInFile::rewind),
    # which asserts on a pipe.
    Tool("soundstretch", "soundstretch", ["soundstretch", "{file}", "-bpm"], MONO_WAV,
         parse_soundstretch, seekable=True),
]


def parse_ffmpeg_log(log: str, rate: int) -> dict:
    r = {}
    m = re.search(r"Number of samples:\s*(\d+)", log)
    if m and rate:
        r["duration"] = round(int(m.group(1)) / rate, 3)
    m = re.search(r"Integrated loudness:\s*I:\s*(-?[\d.]+|-inf) LUFS", log)
    if m:
        lufs = float(m.group(1))
        r["loudness"] = {"integratedLufs": lufs}
        # ReplayGain 2.0 as rsgain/loudgain compute it.
        r["replayGain2Db"] = round(-18.0 - lufs, 2)
    m = re.search(r"LRA:\s*(-?[\d.]+) LU", log)
    if m:
        r.setdefault("loudness", {})["loudnessRangeLu"] = float(m.group(1))
    m = re.search(r"True peak:\s*Peak:\s*(-?[\d.]+|-inf) dBFS", log)
    if m:
        r.setdefault("loudness", {})["truePeakDbtp"] = float(m.group(1))

    starts = [float(x) for x in re.findall(r"silence_start: (-?[\d.]+)", log)]
    ends = [float(x) for x in re.findall(r"silence_end: ([\d.]+)", log)]
    lead = ends[0] if starts and starts[0] <= 0.001 and ends else 0.0
    tail = 0.0
    if "duration" in r and starts and ends and abs(ends[-1] - r["duration"]) < 0.05:
        tail = r["duration"] - starts[-1]
    r["silence"] = {"leadMs": round(lead * 1000), "tailMs": round(tail * 1000)}
    return r


def input_rate(path: str) -> int:
    """Stream sample rate from the container header (no decoding)."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=sample_rate", "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.strip()
    return int(out) if out.isdigit() else 0


def analyse(path: str, silence_db: float) -> dict:
    tools = [t for t in TOOLS if shutil.which(t.binary)]
    result: dict = {"file": path, "skipped": [t.name for t in TOOLS if t not in tools]}

    n = len(tools)
    graph = (f"[0:a]asplit={n + 1}[ana]" + "".join(f"[t{i}]" for i in range(n)) + ";"
             f"[ana]astats=measure_perchannel=none,ebur128=peak=true:framelog=quiet,"
             f"silencedetect=n={silence_db}dB:d=0.01[ana_out]")
    cmd = ["ffmpeg", "-nostdin", "-nostats", "-hide_banner", "-i", path,
           "-filter_complex", graph, "-map", "[ana_out]", "-f", "null", "-"]

    with tempfile.TemporaryDirectory(prefix="reference-") as tmp:
        procs, write_fds, later = [], [], []
        for i, t in enumerate(tools):
            if t.seekable:
                file = os.path.join(tmp, f"{i}.wav")
                later.append((i, [file if a == "{file}" else a for a in t.argv]))
                cmd += ["-map", f"[t{i}]", *t.out_args, "-y", file]
                procs.append(None)
                continue
            r_fd, w_fd = os.pipe()
            p = subprocess.Popen(t.argv, stdin=r_fd, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            os.close(r_fd)
            procs.append(p)
            write_fds.append(w_fd)
            cmd += ["-map", f"[t{i}]", *t.out_args, "-y", f"pipe:{w_fd}"]

        ff = subprocess.Popen(cmd, stderr=subprocess.PIPE, text=True, pass_fds=write_fds)
        for fd in write_fds:     # only ffmpeg may hold the write ends, so readers see EOF
            os.close(fd)
        log = ff.communicate()[1]
        outputs = [p.communicate()[0] if p else "" for p in procs]
        # The file-reading tools, once ffmpeg has written their files.
        if ff.returncode == 0:
            started = [(i, subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, text=True)) for i, argv in later]
            for i, p in started:
                outputs[i] = p.communicate()[0]
                procs[i] = p

    if ff.returncode != 0:
        result["error"] = "ffmpeg: " + log.strip().splitlines()[-1]
        return result
    result["ffmpeg"] = parse_ffmpeg_log(log, input_rate(path))
    for t, p, out in zip(tools, procs, outputs):
        try:
            result[t.name] = t.parse(out) if p.returncode == 0 else {"error": out.strip()[-300:]}
        except (ValueError, KeyError, IndexError, json.JSONDecodeError):
            result[t.name] = {"error": "unparsable output: " + out.strip()[-300:]}
    return result


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--silence-threshold", type=float, default=-100.0,
                    help="dBFS threshold for silencedetect (default -100, as folksentia)")
    args = ap.parse_args()
    results = [analyse(f, args.silence_threshold) for f in args.files]
    json.dump(results if len(results) > 1 else results[0], sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
