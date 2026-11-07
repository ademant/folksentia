#!/usr/bin/env python3
"""Run folksentia over an evaluation dataset and store one JSON per file.

  run.py ballroom                          # variant "default"
  run.py ballroom --variant fast -- --fast # extra folksentia flags after --
  run.py ballroom --per-style 10 -j 2      # quick subset, 2 parallel jobs
  run.py ebu

Results go to $FOLKSENTIA_EVAL_DATA/results/<dataset>/<variant>/<stem>.json,
together with meta.json (flags, binary, git revision). Files that already
have a result are skipped, so an interrupted run can be resumed. A variant's
flags are fixed by its first run; use a new variant name for new flags.
"""

import argparse
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import DEFAULT_BINARY, REPO, data_dir, results_dir

# Flags every run of a dataset needs for its scorer.
BASE_FLAGS = {"ballroom": ["--downbeats"], "ebu": []}


def dataset_files(dataset: str, per_style: int | None) -> list[Path]:
    root = data_dir()
    if dataset == "ballroom":
        audio = root / "ballroom" / "audio"
        if not (audio / ".complete").exists():
            raise SystemExit(f"Ballroom audio missing in {audio}; run fetch_ballroom.py")
        files = []
        for style in sorted(p for p in audio.iterdir() if p.is_dir()):
            wavs = sorted(style.glob("*.wav"))
            if per_style:
                step = max(1, len(wavs) // per_style)  # spread over the style
                wavs = wavs[::step][:per_style]
            files += wavs
        return files
    if dataset == "ebu":
        ebu = root / "ebu"
        files = sorted((ebu / "synthetic").glob("*.wav"))
        files += sorted((ebu / "official").rglob("*.wav"))
        if not files:
            raise SystemExit(f"no signals in {ebu}; run make_ebu_signals.py")
        return files
    raise SystemExit(f"unknown dataset {dataset}")


def git_rev() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def analyse(binary: Path, flags: list[str], wav: Path, dest: Path) -> tuple[Path, float, bool]:
    t0 = time.monotonic()
    p = subprocess.run([str(binary), *flags, str(wav)], capture_output=True, text=True)
    elapsed = time.monotonic() - t0
    try:
        res = json.loads(p.stdout)
    except json.JSONDecodeError:
        res = {"error": f"exit {p.returncode}: {p.stderr.strip()[-500:]}"}
    res["_elapsed"] = round(elapsed, 2)
    tmp = dest.with_suffix(".part")
    tmp.write_text(json.dumps(res) + "\n")
    tmp.rename(dest)
    return wav, elapsed, "error" not in res


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dataset", choices=sorted(BASE_FLAGS))
    ap.add_argument("--variant", default="default")
    ap.add_argument("-j", "--jobs", type=int, default=1)
    ap.add_argument("--per-style", type=int, metavar="N",
                    help="ballroom: only N files per dance style")
    ap.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    args = ap.parse_args(argv)

    if not args.binary.exists():
        raise SystemExit(f"{args.binary} not found; build folksentia first")
    flags = BASE_FLAGS[args.dataset] + extra
    out = results_dir(args.dataset, args.variant)
    out.mkdir(parents=True, exist_ok=True)

    meta_path = out / "meta.json"
    if meta_path.exists():
        old = json.loads(meta_path.read_text())
        if old["flags"] != flags:
            raise SystemExit(f"variant {args.variant!r} was run with flags {old['flags']}, "
                             f"not {flags}; pick another --variant or delete {out}")
    else:
        meta_path.write_text(json.dumps({
            "flags": flags, "binary": str(args.binary.resolve()),
            "git": git_rev(), "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        }, indent=2) + "\n")

    files = dataset_files(args.dataset, args.per_style)
    stems = [f.stem for f in files]
    if len(set(stems)) != len(stems):
        raise SystemExit("duplicate file names in dataset")
    todo = [f for f in files if not (out / f"{f.stem}.json").exists()]
    print(f"{args.dataset}/{args.variant}: {len(files)} files, {len(todo)} to analyse, "
          f"flags {flags}", file=sys.stderr)

    failed, t_start = 0, time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = [pool.submit(analyse, args.binary, flags, f, out / f"{f.stem}.json")
                for f in todo]
        for n, job in enumerate(jobs, 1):
            wav, elapsed, ok = job.result()
            failed += not ok
            eta = (time.monotonic() - t_start) / n * (len(todo) - n)
            print(f"[{n}/{len(todo)}] {wav.name} {elapsed:.1f}s"
                  f"{'' if ok else ' FAILED'}  (eta {eta / 60:.0f} min)",
                  file=sys.stderr, flush=True)
    print(f"done, {failed} failed; results in {out}", file=sys.stderr)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
