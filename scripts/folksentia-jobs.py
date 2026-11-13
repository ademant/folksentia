#!/usr/bin/env python3
"""Run folksentia over a folksonic job list (offline analysis).

  folksentia-jobs --jobs jobs.json --music-root ~/Musik --out results.jsonl [-j N]

The job list comes from `tinysonic analysis export`; the result file goes to
`tinysonic analysis import`. Each job runs in its own folksentia process, N at
a time, so a crash or an out-of-memory kill only fails that job. The output
gets one JSON line per job and can be resumed: jobs already in it are skipped.
See README.md, "Offline analysis for folksonic".
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path, PurePosixPath

JOBS_FORMAT = "tinysonic-analysis-jobs"
JOBS_VERSION = 1
SCHEMA = 1


def fail(msg):
    print(f"folksentia-jobs: {msg}", file=sys.stderr)
    sys.exit(2)


def default_binary():
    """folksentia next to this script (build or install dir), else on PATH."""
    here = Path(sys.argv[0]).resolve().parent / "folksentia"
    if here.is_file() and os.access(here, os.X_OK):
        return str(here)
    return shutil.which("folksentia") or "folksentia"


def tool_version(binary):
    try:
        out = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=60).stdout
    except OSError as e:
        fail(f"cannot run {binary}: {e}")
    first = out.splitlines()[0].split() if out else []
    if len(first) != 2 or first[0] != "folksentia":
        fail(f"{binary} --version gave no version; is it folksentia 0.1.0 or newer?")
    return first[1]


def load_jobs(path):
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        fail(f"{path}: {e}")
    if doc.get("format") != JOBS_FORMAT or doc.get("version") != JOBS_VERSION:
        fail(f"{path}: not a {JOBS_FORMAT} version {JOBS_VERSION} file")
    if doc.get("tool") != "folksentia" or doc.get("schema") != SCHEMA:
        fail(f"{path}: jobs for {doc.get('tool')} schema {doc.get('schema')}, expected folksentia schema {SCHEMA}")
    jobs, seen = [], set()
    for job in doc.get("jobs") or []:
        if not isinstance(job, dict) or not isinstance(job.get("id"), str) or not isinstance(job.get("path"), str):
            fail(f"{path}: job without id or path: {json.dumps(job)[:200]}")
        if job["id"] not in seen:  # a song once, the first job wins
            seen.add(job["id"])
            jobs.append(job)
    return doc.get("options") or {}, jobs


def folksentia_args(options):
    """The flags folksonic passes for a local run (folksentiaArgs)."""
    args = []
    if options.get("downbeats"):
        args.append("--downbeats")
    if options.get("waveform"):
        args.append(f"--waveform={int(options['waveform'])}")
    if options.get("fingerprint"):
        args.append("--fingerprint")
    return args


def done_ids(out):
    """IDs already in the output. A half-written last line (an interrupted
    run) is cut off so that new lines start cleanly."""
    if not out.exists():
        return set()
    data = out.read_bytes()
    end = data.rfind(b"\n") + 1
    if end < len(data):
        with open(out, "r+b") as f:
            f.truncate(end)
        print(f"folksentia-jobs: removed a half-written last line from {out}", file=sys.stderr)
    ids = set()
    for line in data[:end].splitlines():
        try:
            ids.add(json.loads(line)["job"]["id"])
        except (ValueError, KeyError, TypeError):
            pass  # import reports bad lines
    return ids


def safe_path(root, rel):
    """root/rel, or None when rel is absolute or leaves root."""
    p = PurePosixPath(rel)
    if not rel or p.is_absolute() or ".." in p.parts or "\\" in rel:
        return None
    return root.joinpath(*p.parts)


def stamp(path):
    st = path.stat()
    # Go's UnixMilli(): nanoseconds truncated to milliseconds.
    return {"size": st.st_size, "mtimeMs": st.st_mtime_ns // 1_000_000}


def run_job(job, root, args, binary, version, ignore_mtime, timeout, procs):
    """The result line for one job."""
    path = safe_path(root, job["path"])
    if path is None:
        return {"job": job, "skipped": "missing"}, "refused (absolute or outside the music root)"
    try:
        if not path.is_file():
            return {"job": job, "skipped": "missing"}, "missing"
        local = stamp(path)
    except OSError:
        return {"job": job, "skipped": "missing"}, "missing"
    if local["size"] != job.get("size") or (not ignore_mtime and local["mtimeMs"] != job.get("mtimeMs")):
        return {"job": job, "local": local, "skipped": "changed"}, "changed"

    def error(msg):
        return {"schema": SCHEMA, "file": job["path"], "error": msg}

    try:
        p = subprocess.Popen([binary, *args, "--", str(path)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True)
    except OSError as e:
        return {"job": job, "local": local, "toolVersion": version, "result": error(str(e))}, "error"
    if not procs.add(p):  # interrupted meanwhile
        p.kill()
        p.communicate()
        return None, "interrupted"
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        result, status = error(f"folksentia timed out after {timeout:g} s"), "error"
    else:
        try:
            result = json.loads(out)
            status = "error" if "error" in result else "ok"
        except ValueError:
            if p.returncode < 0:
                msg = f"folksentia killed by signal {-p.returncode}"
                if -p.returncode == signal.SIGKILL:
                    msg += " (out of memory?)"
            else:
                msg = f"folksentia exited with {p.returncode}: {err.decode('utf-8', 'replace').strip()[-300:]}"
            result, status = error(msg), "error"
    finally:
        procs.discard(p)
    if isinstance(result, dict):
        # The job's relative path, not this machine's path.
        result["file"] = job["path"]
        result.pop("file_base64", None)
    return {"job": job, "local": local, "toolVersion": version, "result": result}, status


class Procs:
    """Running child processes, to stop them on Ctrl-C."""

    def __init__(self):
        self.lock, self.items, self.stopped = threading.Lock(), set(), False

    def add(self, p):
        """Track p; False once stopped."""
        with self.lock:
            if self.stopped:
                return False
            self.items.add(p)
            return True

    def discard(self, p):
        with self.lock:
            self.items.discard(p)

    def kill_all(self):
        with self.lock:
            self.stopped = True
            for p in self.items:
                p.kill()


def main():
    ap = argparse.ArgumentParser(
        description="Run folksentia over a folksonic job list (tinysonic analysis export) "
                    "and write results for tinysonic analysis import.")
    ap.add_argument("--jobs", required=True, type=Path, help="job list from tinysonic analysis export")
    ap.add_argument("--music-root", required=True, type=Path, help="local copy of the music folder")
    ap.add_argument("--out", required=True, type=Path, help="result file (JSON lines); appended to, resumable")
    ap.add_argument("-j", "--parallel", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="folksentia processes at a time (default: half the CPUs)")
    ap.add_argument("--ignore-mtime", action="store_true",
                    help="compare file sizes only (for copies that did not keep modification times)")
    ap.add_argument("--timeout", type=float, default=0, help="seconds per track, 0 for none (default)")
    ap.add_argument("--folksentia", default=None, help="folksentia binary (default: next to this script, else PATH)")
    a = ap.parse_args()
    if a.parallel < 1:
        fail("-j needs at least 1")
    if not a.music_root.is_dir():
        fail(f"{a.music_root}: not a directory")

    binary = a.folksentia or default_binary()
    version = tool_version(binary)
    options, jobs = load_jobs(a.jobs)
    args = folksentia_args(options)
    done = done_ids(a.out)
    todo = [j for j in jobs if j["id"] not in done]
    print(f"folksentia-jobs: {len(jobs)} jobs, {len(jobs) - len(todo)} already done, "
          f"{len(todo)} to run with folksentia {version}, {a.parallel} at a time", file=sys.stderr)

    counts = {}
    procs = Procs()
    start = time.monotonic()
    root = a.music_root.expanduser()
    with open(a.out, "a", encoding="utf-8") as out, ThreadPoolExecutor(a.parallel) as pool:
        futures = {pool.submit(run_job, j, root, args, binary, version, a.ignore_mtime, a.timeout or None, procs): j
                   for j in todo}
        try:
            for n, fut in enumerate(as_completed(futures), 1):
                line, status = fut.result()
                if line is None:
                    continue
                # One writer, one complete line at a time.
                out.write(json.dumps(line, separators=(",", ":")) + "\n")
                out.flush()
                counts[status] = counts.get(status, 0) + 1
                job = futures[fut]
                if status != "ok":
                    print(f"[{n}/{len(todo)}] {job['path']}: {status}"
                          + (f": {line['result']['error']}" if status == "error" else ""), file=sys.stderr)
                elif n % 25 == 0 or n == len(todo):
                    rate = n / (time.monotonic() - start)
                    print(f"[{n}/{len(todo)}] {rate * 60:.1f} tracks/min", file=sys.stderr)
        except KeyboardInterrupt:
            for f in futures:
                f.cancel()
            procs.kill_all()
            print("folksentia-jobs: interrupted; run again to resume", file=sys.stderr)
            return 130

    summary = ", ".join(f"{v} {k}" for k, v in sorted(counts.items())) or "nothing to do"
    print(f"folksentia-jobs: {summary}", file=sys.stderr)
    return 1 if counts.get("error") else 0


if __name__ == "__main__":
    sys.exit(main())
