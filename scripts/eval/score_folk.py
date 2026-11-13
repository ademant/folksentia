#!/usr/bin/env python3
"""Score folksentia's rhythm object against the labelled folk tunes.

  score_folk.py --music-root ~/Musik [--binary builddir/folksentia] [-j 4]

Runs folksentia on every labelled file found under the music root and
compares, per tune:

- bar: rhythm.barSec within the label's tolerance (default 4 %) of bar_sec;
- grouping: the beat starts of rhythm.grouping and of the label, as fractions
  of the bar, match for some rotation (within half a pulse);
- tracker: the beat tracker's bpm, for comparison with the labelled bars per
  minute.

Standard library only. Labels: folk_labels.json next to this script.
"""

import argparse
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import DEFAULT_BINARY

LABELS = Path(__file__).resolve().parent / "folk_labels.json"


def starts(grouping):
    """Beat starts as fractions of the bar."""
    total, pos, out = sum(grouping), 0, []
    for g in grouping:
        out.append(pos / total)
        pos += g
    return out


def same_grouping(a, b, tolerance):
    """True when groupings a and b put beats at the same places in the bar,
    for some rotation (the bar start is not known)."""
    sa, sb = starts(a), starts(b)
    if len(sa) != len(sb):
        return False
    for shift in sa:
        rotated = sorted((x - shift) % 1.0 for x in sa)
        if all(min(abs(x - y), 1 - abs(x - y)) <= tolerance for x, y in zip(rotated, sorted(sb))):
            return True
    return False


def analyse(binary, path):
    p = subprocess.run([binary, str(path)], capture_output=True)
    try:
        return json.loads(p.stdout)
    except ValueError:
        return {"error": p.stderr.decode("utf-8", "replace").strip()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--music-root", required=True, type=Path)
    ap.add_argument("--binary", default=str(DEFAULT_BINARY))
    ap.add_argument("-j", type=int, default=4)
    a = ap.parse_args()

    tunes = json.loads(LABELS.read_text(encoding="utf-8"))["tunes"]
    found = [(t, a.music_root / t["file"]) for t in tunes if (a.music_root / t["file"]).is_file()]
    missing = [t["file"] for t in tunes if not (a.music_root / t["file"]).is_file()]
    if missing:
        print(f"not found under {a.music_root}: {', '.join(missing)}", file=sys.stderr)
    with ThreadPoolExecutor(a.j) as pool:
        results = list(pool.map(lambda tp: analyse(a.binary, tp[1]), found))

    rows, bar_ok, grouping_ok, grouping_n = [], 0, 0, 0
    for (t, _), r in zip(found, results):
        rh = r.get("rhythm") or {}
        bar = rh.get("barSec")
        tol = t.get("bar_tolerance", 0.04)
        bok = bar is not None and abs(bar - t["bar_sec"]) <= tol * t["bar_sec"]
        bar_ok += bok
        g = rh.get("grouping")
        if len(t["grouping"]) > 1 and len(set(t["grouping"])) > 1:  # uneven: a real grouping test
            grouping_n += 1
            pulses = sum(g) if g else 1
            gok = bool(g) and same_grouping(g, t["grouping"], 0.5 / pulses + 0.01)
            grouping_ok += gok
        else:
            gok = None if not g else same_grouping(g, t["grouping"], 0.05)
        tracker = r.get("bpm") or 0
        rows.append((t["file"][:30], t["dance"], t["meter"], t["bar_sec"], bar, bok,
                     "+".join(map(str, t["grouping"])), "+".join(map(str, g)) if g else "-", gok,
                     60 / t["bar_sec"], tracker, r.get("error")))

    print(f"| file | dance | meter | bar (label) | barSec | ok | grouping (label) | grouping | ok | bars/min (label) | tracker bpm |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for f, d, m, lb, b, bok, lg, g, gok, lbm, tr, err in rows:
        if err:
            print(f"| {f} | {d} | {m} | error: {err} |||||||| ")
            continue
        fmt = lambda v: "-" if v is None else ("yes" if v else "no")
        print(f"| {f} | {d} | {m} | {lb:.3f} | {'-' if b is None else f'{b:.3f}'} | {fmt(bok)} | {lg} | {g} | {fmt(gok)} | {lbm:.1f} | {tr:.1f} |")
    print(f"\nbar: {bar_ok}/{len(rows)} within tolerance; uneven grouping: {grouping_ok}/{grouping_n} matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
