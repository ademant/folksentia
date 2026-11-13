#!/usr/bin/env python3
"""Test folksentia-jobs: a job list in folksonic's export format, run against
a temporary music folder, checked line by line, then resumed.

  tests/jobs.py builddir/folksentia builddir/folksentia-jobs
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from smoke import check, clicks, failures, write_wav  # noqa: E402

BINARY, WRAPPER = (os.path.abspath(a) for a in sys.argv[1:3])


def job(id, path, root, **override):
    p = root / path
    st = p.stat() if p.exists() else None
    j = {"id": id, "path": path, "size": st.st_size if st else 1,
         "mtimeMs": st.st_mtime_ns // 1_000_000 if st else 1, "durationMs": 1000,
         "features": ["bpm", "beats", "key", "loudness", "silence", "waveform"]}
    j.update(override)
    return j


def run(*args):
    return subprocess.run([sys.executable, WRAPPER, "--folksentia", BINARY, *args], capture_output=True, text=True)


def main():
    tmp = Path(tempfile.mkdtemp(prefix="folksentia-jobs-"))
    root = tmp / "music"
    (root / "Artist" / "Album").mkdir(parents=True)
    mono = clicks(seconds=10)
    write_wav(str(root / "Artist" / "Album" / "01 Clicks.wav"), 44100, 2, [s for s in mono for _ in range(2)])
    write_wav(str(root / "Artist" / "Album" / "02 Silence.wav"), 48000, 1, [0.0] * 48000 * 2)
    (root / "Artist" / "Album" / "03 Broken.wav").write_bytes(b"RIFF not really a wav file")

    jobs = [
        job("so-ok", "Artist/Album/01 Clicks.wav", root),
        job("so-silent", "Artist/Album/02 Silence.wav", root),
        job("so-broken", "Artist/Album/03 Broken.wav", root),
        job("so-missing", "Artist/Album/04 Gone.wav", root),
        job("so-changed", "Artist/Album/01 Clicks.wav", root, size=123),
        job("so-escape", "../outside.wav", root),
        job("so-ok", "Artist/Album/02 Silence.wav", root),  # duplicate id: first wins
    ]
    jobs_file = tmp / "jobs.json"
    jobs_file.write_text(json.dumps({
        "format": "tinysonic-analysis-jobs", "version": 1, "created": "2026-11-13T10:00:00Z",
        "tool": "folksentia", "schema": 1,
        "options": {"downbeats": True, "waveform": 150}, "jobs": jobs}))
    out = tmp / "results.jsonl"

    p = run("--jobs", str(jobs_file), "--music-root", str(root), "--out", str(out), "-j", "3")
    check(p.returncode == 1, f"first run: exit 1 because one file is broken (got {p.returncode}: {p.stderr[-300:]})")
    raw = out.read_text().splitlines()
    lines = {json.loads(l)["job"]["id"]: json.loads(l) for l in raw}
    check(len(raw) == 6 and len(lines) == 6, f"one line per distinct job ({len(raw)} lines)")

    version = subprocess.run([BINARY, "--version"], capture_output=True, text=True).stdout.split()[1]
    ok = lines["so-ok"]
    check(ok["job"] == jobs[0], "ok: job copied unchanged")
    check(ok["local"] == {"size": jobs[0]["size"], "mtimeMs": jobs[0]["mtimeMs"]}, "ok: local stamp matches the job")
    check(ok["toolVersion"] == version, f"ok: toolVersion {ok['toolVersion']}")
    r = ok["result"]
    check(r["schema"] == 1 and r["file"] == "Artist/Album/01 Clicks.wav", "ok: schema 1, file is the job path")
    check(abs(r["bpm"] - 120) < 2 and r["beats"] and "downbeatIndexes" in r and r["waveform"]["bins"] == 150,
          "ok: options applied (beats, downbeats, waveform 150)")
    check("fingerprint" not in r, "ok: no fingerprint when not requested")
    check(lines["so-silent"]["result"]["bpm"] is None, "silent: analysed, bpm null")
    check("error" in lines["so-broken"]["result"] and lines["so-broken"]["toolVersion"] == version,
          "broken: result with error")
    check(lines["so-missing"] == {"job": jobs[3], "skipped": "missing"}, "missing: skipped missing")
    ch = lines["so-changed"]
    check(ch["skipped"] == "changed" and ch["local"]["size"] != 123, "changed: skipped changed with local stamp")
    check(lines["so-escape"]["skipped"] == "missing", "path with ..: refused as missing")

    # Resume after an interrupted write: the half line goes, nothing is redone.
    with open(out, "a") as f:
        f.write('{"job": {"id": "so-half"')
    p = run("--jobs", str(jobs_file), "--music-root", str(root), "--out", str(out))
    check(p.returncode == 0 and out.read_text().splitlines() == raw,
          f"resume: half line removed, no job run twice ({p.stderr.strip().splitlines()[-1:]})")

    # --ignore-mtime: a copy with another mtime is analysed, not skipped.
    os.utime(root / "Artist" / "Album" / "01 Clicks.wav", (1, 1))
    out2 = tmp / "results2.jsonl"
    p = run("--jobs", str(jobs_file), "--music-root", str(root), "--out", str(out2), "--ignore-mtime")
    l2 = {json.loads(l)["job"]["id"]: json.loads(l) for l in out2.read_text().splitlines()}
    check("result" in l2["so-ok"], "--ignore-mtime: other mtime still analysed")
    p = run("--jobs", str(jobs_file), "--music-root", str(root), "--out", str(tmp / "results3.jsonl"))
    l3 = {json.loads(l)["job"]["id"]: json.loads(l) for l in (tmp / "results3.jsonl").read_text().splitlines()}
    check(l3["so-ok"].get("skipped") == "changed", "without --ignore-mtime: other mtime is changed")

    # Bad job files.
    bad = tmp / "bad.json"
    bad.write_text(json.dumps({"format": "something-else", "version": 1}))
    p = run("--jobs", str(bad), "--music-root", str(root), "--out", str(tmp / "x.jsonl"))
    check(p.returncode == 2, "wrong format: exit 2")

    print(f"\n{len(failures)} failure(s)" if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
