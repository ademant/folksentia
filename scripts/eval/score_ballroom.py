#!/usr/bin/env python3
"""Score folksentia's tempo, beats and downbeats on the Ballroom dataset.

  .venv/bin/python score_ballroom.py default fast     # one table per variant
  .venv/bin/python score_ballroom.py default --csv per-file.csv

Metrics, per dance style and overall (mean over files):
  Acc1   tempo within 4% of the annotated tempo
  Acc2   same, also accepting x2, x3, x1/2, x1/3 (octave errors)
  x2/x½  share of files whose tempo is double / half the reference
  BeatF  beat F-measure, +-70 ms window (mir_eval; first 5 s ignored)
  CMLt   correct metrical level, total continuity (mir_eval)
  AMLt   allowed metrical levels (double/half/offbeat), total continuity
  DbF    downbeat F-measure, +-70 ms; files with null downbeats score 0
  Db?    share of files where folksentia gave no downbeats (null)
  meter  time signature of the annotations (beats per bar)
  s/file mean analysis time

Needs numpy and mir_eval (see requirements.txt).
"""

import argparse
import csv
import sys
import warnings
from collections import defaultdict

import mir_eval
import numpy as np

from common import data_dir, load_results

STYLE_ORDER = ["Waltz", "VienneseWaltz", "Tango", "ChaChaCha", "Jive", "Quickstep",
               "Rumba-American", "Rumba-International", "Rumba-Misc", "Samba"]


def load_reference():
    root = data_dir() / "ballroom"
    ref = {}
    for wav in (root / "audio").glob("*/*.wav"):
        stem = wav.stem
        tempo = float((root / "tempo" / f"{stem}.bpm").read_text().split()[0])
        rows = np.loadtxt(root / "beats" / f"{stem}.beats", ndmin=2)
        beats, pos = rows[:, 0], rows[:, 1].astype(int)
        ref[stem] = {"style": wav.parent.name, "tempo": tempo, "beats": beats,
                     "downbeats": beats[pos == 1], "meter": int(pos.max())}
    return ref


def tempo_class(est, ref):
    """Which tempo relation the estimate has to the reference, within 4%."""
    for name, factor in (("1", 1), ("x2", 2), ("x½", 0.5), ("x3", 3), ("x⅓", 1 / 3),
                         ("x1.5", 1.5), ("x⅔", 2 / 3)):
        if abs(est - ref * factor) <= 0.04 * ref * factor:
            return name
    return "other"


def score_file(res, ref):
    if "error" in res:
        return None
    est_beats = np.array(res.get("beats") or [], dtype=float)
    cls = tempo_class(res["bpm"], ref["tempo"])
    s = {
        "bpm": res["bpm"], "ref_bpm": ref["tempo"], "tempo_class": cls,
        "acc1": float(cls == "1"),
        "acc2": float(cls in ("1", "x2", "x½", "x3", "x⅓")),
        "x2": float(cls == "x2"), "xhalf": float(cls == "x½"),
        "elapsed": res.get("_elapsed", float("nan")),
    }
    rb, eb = mir_eval.beat.trim_beats(ref["beats"]), mir_eval.beat.trim_beats(est_beats)
    s["beat_f"] = mir_eval.beat.f_measure(rb, eb)
    _, s["cmlt"], _, s["amlt"] = mir_eval.beat.continuity(rb, eb)

    idx = res.get("downbeatIndexes")
    s["db_null"] = float(idx is None)
    est_db = est_beats[np.array(idx, dtype=int)] if idx else np.array([])
    with warnings.catch_warnings():  # null downbeats are expected; scored as 0
        warnings.simplefilter("ignore", UserWarning)
        s["db_f"] = mir_eval.beat.f_measure(mir_eval.beat.trim_beats(ref["downbeats"]),
                                            mir_eval.beat.trim_beats(est_db))
    return s


COLUMNS = [("n", None), ("meter", None), ("Acc1", "acc1"), ("Acc2", "acc2"),
           ("x2", "x2"), ("x½", "xhalf"), ("BeatF", "beat_f"), ("CMLt", "cmlt"),
           ("AMLt", "amlt"), ("DbF", "db_f"), ("Db?", "db_null"), ("s/file", "elapsed")]


def table(variant, scored, ref, failed):
    by_style = defaultdict(list)
    for stem, s in scored.items():
        by_style[ref[stem]["style"]].append((stem, s))
    styles = [st for st in STYLE_ORDER if st in by_style] + \
             sorted(set(by_style) - set(STYLE_ORDER))

    print(f"\n### {variant}\n")
    if not scored:
        print("no analysed files yet")
        return
    print("| style | " + " | ".join(c for c, _ in COLUMNS) + " |")
    print("|---|" + "---:|" * len(COLUMNS))

    def row(label, items):
        cells = [str(len(items)),
                 "/".join(sorted({str(ref[st]["meter"]) for st, _ in items}))]
        for _, key in COLUMNS[2:]:
            v = np.mean([s[key] for _, s in items])
            cells.append(f"{v:.1f}" if key == "elapsed" else f"{100 * v:.0f}")
        print(f"| {label} | " + " | ".join(cells) + " |")

    for st in styles:
        row(st, by_style[st])
    row("**all**", [x for st in styles for x in by_style[st]])
    if failed:
        print(f"\n{len(failed)} file(s) failed to analyse: {', '.join(sorted(failed)[:10])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("variants", nargs="*", default=["default"])
    ap.add_argument("--csv", help="also write per-file scores of all variants here")
    args = ap.parse_args()

    ref = load_reference()
    print("Scores in percent. Acc1/Acc2: tempo within 4% (Acc2 forgives octave "
          "errors). BeatF/CMLt/AMLt/DbF: mir_eval, ±70 ms.")
    rows = []
    for variant in args.variants:
        results = load_results("ballroom", variant)
        unknown = set(results) - set(ref)
        if unknown:
            print(f"warning: {len(unknown)} results without reference, ignored",
                  file=sys.stderr)
        scored, failed = {}, []
        for stem, res in results.items():
            if stem not in ref:
                continue
            s = score_file(res, ref[stem])
            if s is None:
                failed.append(stem)
            else:
                scored[stem] = s
                rows.append({"variant": variant, "file": stem,
                             "style": ref[stem]["style"], **s})
        table(variant, scored, ref, failed)

    if args.csv and rows:
        with open(args.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
