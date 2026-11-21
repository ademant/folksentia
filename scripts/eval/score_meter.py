#!/usr/bin/env python3
"""Score folksentia's meter estimates against meter_labels.json.

  score_meter.py --music-root ~/Musik [--variant default] [--binary builddir/folksentia] [-j 8] [--sample N]
                 [--reference ref.json] [--by dance|meter] [--disagreements] [--markdown]

Runs `folksentia --downbeats` on every labelled file found under the music
root (results cached in $FOLKSENTIA_EVAL_DATA/results/meter/<variant>/, so
reruns only analyse new files; delete the directory after changing
folksentia) and scores three estimates of the beats per bar:

- rhythm.barPulses (the bar in onset pulses),
- downbeatMeter (beats per bar used for downbeatIndexes),
- rhythm.microtiming.beatsPerBar.

Per label group: "type": the answer has the label's meter type (binary 2/4,
4/4; ternary 3; six 6; odd 5, 7, 8 = 3+3+2, 11 ...; for an odd label also the
exact meter or twice it); "exact": the label's beats, twice or half of them
(a laridé 4/4 answered 2 or 8 counts); "none": no answer (null or 0). Below,
"right": of the tracks an estimator calls binary (ternary, ...), the share
whose label has that type.

--reference adds other estimators for comparison, from a JSON map of file
(relative to the music root) or MusicBrainz id -> {"name": beats per bar}.
--disagreements lists tracks where every estimator that answered has the
wrong meter type: candidates for checking the label by ear.
Standard library only.
"""

import argparse
import collections
import json
import random
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import DEFAULT_BINARY, results_dir

LABELS = Path(__file__).resolve().parent / "meter_labels.json"
TYPES = ("binary", "ternary", "six", "odd")


def answer_type(n):
    if not n or n < 2:
        return None
    if n in (2, 4, 8, 16):
        return "binary"
    if n in (3, 9):
        return "ternary"
    if n in (6, 12):
        return "six"
    return "odd"


def beats(meter):
    return int(meter.split("/")[0])


def type_ok(label, n):
    t = answer_type(n)
    if t is None:
        return False
    if label["type"] == "odd":
        return t == "odd" or n in (beats(label["meter"]), 2 * beats(label["meter"]))
    return t == label["type"]


def exact_ok(label, n):
    b = beats(label["meter"])
    return bool(n) and (n in (b, 2 * b) or 2 * n == b)


def estimates(r):
    rh = r.get("rhythm") or {}
    return {"barPulses": rh.get("barPulses") or None,
            "downbeatMeter": r.get("downbeatMeter") or None,
            "microtiming": (rh.get("microtiming") or {}).get("beatsPerBar") or None}


def analyse(binary, path, out):
    if out.is_file():
        return json.loads(out.read_text())
    p = subprocess.run([binary, "--downbeats", str(path)], capture_output=True)
    try:
        r = json.loads(p.stdout)
    except ValueError:
        r = {"error": p.stderr.decode("utf-8", "replace").strip()[:300]}
    r.pop("beats", None)  # keep the cache small
    r.pop("downbeatIndexes", None)
    out.write_text(json.dumps(r))
    return r


def pct(a, b):
    return f"{100 * a / b:.0f} %" if b else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--music-root", required=True, type=Path)
    ap.add_argument("--binary", default=str(DEFAULT_BINARY))
    ap.add_argument("--variant", default="default")
    ap.add_argument("--labels", type=Path, default=LABELS)
    ap.add_argument("--reference", type=Path)
    ap.add_argument("--by", choices=("meter", "dance"), default="meter")
    ap.add_argument("--disagreements", action="store_true")
    ap.add_argument("--markdown", action="store_true", help="only the summary table, for the README")
    ap.add_argument("--sample", type=int, help="score only this many labelled tracks, picked at random (seed 1)")
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args()

    tunes = json.loads(a.labels.read_text(encoding="utf-8"))["tunes"]
    found = [t for t in tunes if (a.music_root / t["file"]).is_file()]
    if a.sample and a.sample < len(found):
        found = sorted(random.Random(1).sample(found, a.sample), key=lambda t: t["file"])
    if len(found) < len(tunes):
        print(f"{len(tunes) - len(found)} of {len(tunes)} labelled files not under {a.music_root}", file=sys.stderr)
    cache = results_dir("meter", a.variant)
    cache.mkdir(parents=True, exist_ok=True)
    meta = cache / "meta.json"
    if not meta.is_file():
        meta.write_text(json.dumps({"binary": a.binary, "flags": ["--downbeats"]}))
    key = lambda t: t["mbid"] + "-" + Path(t["file"]).stem[:40].replace("/", "_")
    with ThreadPoolExecutor(a.j) as pool:
        results = list(pool.map(lambda t: analyse(a.binary, a.music_root / t["file"], cache / f"{key(t)}.json"),
                                found))

    ref = json.loads(a.reference.read_text()) if a.reference else {}
    names = ["barPulses", "downbeatMeter", "microtiming"]
    rows = []
    for t, r in zip(found, results):
        if "error" in r:
            continue
        e = estimates(r)
        extra = ref.get(t["file"]) or ref.get(t["mbid"]) or {}
        for k, v in extra.items():
            e[k] = v or None
            if k not in names:
                names.append(k)
        rows.append((t, e, r))
    errors = len(found) - len(rows)

    groups = collections.defaultdict(list)
    for t, e, r in rows:
        groups[t["meter"] if a.by == "meter" else t["dance"]].append((t, e))
    order = sorted(groups, key=lambda g: (-len(groups[g]), g))
    ok_names = [n for n in names if any(n in e for _, e, _ in rows)]

    head = "| " + a.by + " | tracks | " + " | ".join(f"{n} type / exact / none" for n in ok_names) + " |"
    print(head)
    print("|---|---:|" + "---:|" * len(ok_names))
    for g in order + ["all"]:
        items = [(t, e) for t, e, _ in rows] if g == "all" else groups[g]
        cells = []
        for n in ok_names:
            have = [(t, e.get(n)) for t, e in items if n in e]
            cells.append(f"{pct(sum(type_ok(t, v) for t, v in have), len(have))} / "
                         f"{pct(sum(exact_ok(t, v) for t, v in have), len(have))} / "
                         f"{pct(sum(not v for _, v in have), len(have))}")
        print(f"| {'**all**' if g == 'all' else g} | {len(items)} | " + " | ".join(cells) + " |")
    if a.markdown:
        return 0

    print("\n| answer type | " + " | ".join(f"{n} right (of)" for n in ok_names) + " |")
    print("|---|" + "---:|" * len(ok_names))
    for ty in TYPES:
        cells = []
        for n in ok_names:
            said = [t for t, e, _ in rows if answer_type(e.get(n)) == ty]
            cells.append(f"{pct(sum(t['type'] == ty for t in said), len(said))} ({len(said)})")
        print(f"| {ty} | " + " | ".join(cells) + " |")
    print(f"\n{len(rows)} tracks scored, {errors} errors; labels: {a.labels.name}, results: {cache}")

    if a.disagreements:
        print("\n## Every answer has the wrong meter type\n")
        print("| label | dance | " + " | ".join(ok_names) + " | file |")
        print("|---|---|" + "---:|" * len(ok_names) + "---|")
        for t, e, _ in sorted(rows, key=lambda x: (x[0]["dance"], x[0]["file"])):
            answered = [e[n] for n in ok_names if e.get(n)]
            if len(answered) >= 2 and not any(type_ok(t, v) for v in answered):
                print(f"| {t['meter']} | {t['dance']} | " + " | ".join(str(e.get(n) or "-") for n in ok_names)
                      + f" | {t['file']} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
