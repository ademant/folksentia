#!/usr/bin/env python3
"""Check folksentia's EBU R128 loudness and true peak against EBU Tech 3341.

  run.py ebu && check_ebu.py [variant]

Tolerances from Tech 3341: integrated loudness +-0.1 LU, true peak
+0.2/-0.4 dB. Exits 1 if any case fails. Uses only the standard library.
"""

import argparse
import json
import re
import sys

from common import data_dir, load_results

# Expected values for files from the official EBU zip (ebu/official/), by
# Tech 3341 case number. Only cases folksentia can measure (stereo, integrated
# loudness and true peak) are listed.
OFFICIAL = {
    1: {"integratedLufs": -23.0}, 2: {"integratedLufs": -33.0},
    3: {"integratedLufs": -23.0}, 4: {"integratedLufs": -23.0},
    5: {"integratedLufs": -23.0},
    15: {"truePeakDbtp": -6.0}, 16: {"truePeakDbtp": -6.0},
    17: {"truePeakDbtp": -6.0}, 18: {"truePeakDbtp": -6.0},
}
TOLERANCE = {"integratedLufs": (-0.1, 0.1), "truePeakDbtp": (-0.4, 0.2)}


def expected_values() -> dict:
    exp = json.loads((data_dir() / "ebu" / "synthetic" / "expected.json").read_text())
    for p in (data_dir() / "ebu" / "official").rglob("*.wav"):
        m = re.search(r"3341-(\d+)\D", p.name)
        if m and int(m.group(1)) in OFFICIAL:
            exp[p.stem] = OFFICIAL[int(m.group(1))] | {"source": "official EBU file"}
    return exp


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("variant", nargs="?", default="default")
    args = ap.parse_args()

    results = load_results("ebu", args.variant)
    failures = 0
    print(f"| case | field | expected | measured | diff | result |")
    print(f"|---|---|---|---|---|---|")
    for case, exp in sorted(expected_values().items()):
        res = results.get(case)
        if res is None:
            print(f"| {case} | | | not run | | MISSING |")
            failures += 1
            continue
        if "error" in res:
            print(f"| {case} | | | {res['error']} | | FAIL |")
            failures += 1
            continue
        for field, want in exp.items():
            if field == "source":
                continue
            got = (res.get("loudness") or {}).get(field)
            if want is None or got is None:
                ok, diff = want is got, ""
            else:
                lo, hi = TOLERANCE[field]
                d = got - want
                ok, diff = lo <= d <= hi, f"{d:+.2f}"
            failures += not ok
            fmt = lambda v: "null" if v is None else f"{v:.2f}"
            print(f"| {case} | {field} | {fmt(want)} | {fmt(got)} | {diff} | "
                  f"{'ok' if ok else 'FAIL'} |")
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
