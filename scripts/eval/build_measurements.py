#!/usr/bin/env python3
"""Collect per-tune measurements and dance tags into a JSONL file.

  build_measurements.py [--runs-dir ~/folksentia-cloud/meter-runs]
                        [--library ~/folksentia-cloud/library.json]
                        [--labels meter_labels.json] [-o measurements.jsonl] [--long]

Reads the existing meter runs (one JSON per run, as written by the
dance-meter-check scripts), the library's MusicBrainz and dancebrainz tags and
the folk meter labels. One line per tune:

  path, mbid, artist, album, title, date   from the library file tags
  dance                                   current dancebrainz dance, if any
  dancebrainz                             dances, meters, structures, bpm, raters
  label                                   meter label, if the tune has one
  finding                                 phrase-lag meter result from the runs
  runs                                    run name -> {dance, ...that run's measures}

`finding` holds the 4-bar autocorrelation test: the similarity at lag 12 (four
3-beat bars) against 8 (four 2-beat bars) and 16 (eight 2-beat bars), per
feature, and the resulting meter guess. --long writes one line per (tune, run)
instead, with source and measures.
No audio, no re-analysis; keyed by the path relative to the music root.
Standard library only.
"""

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAG_RUNS = ("pulselag", "valse", "blag_all")
LAG_KEYS = (8, 12, 16)


def lag_at(feature, k):
    if isinstance(feature, dict):
        return feature.get(str(k))
    if isinstance(feature, list) and len(feature) >= k:
        return feature[k - 1]
    return None


def finding_for(row):
    for source in LAG_RUNS:
        run = row["runs"].get(source)
        if not run:
            continue
        features = {}
        for name in ("chroma", "onset"):
            f = run.get(name)
            if f is None and isinstance(run.get("lags"), dict):
                f = run["lags"].get(name)
            values = {str(k): lag_at(f, k) for k in LAG_KEYS} if f else {}
            if len(values) < len(LAG_KEYS) or any(v is None for v in values.values()):
                continue
            values["score"] = round(values["12"] - max(values["8"], values["16"]), 4)
            features[name] = {"source": source, **values}
        if not features:
            continue
        score = round(sum(f["score"] for f in features.values()), 4)
        meter = "3/4" if score > 0 else "binary"
        label = row.get("label") or {}
        return {"phraseMeter": meter, "score": score, "features": features,
                "agreesWithLabel": None if not label else
                (meter == "3/4") == (label.get("type") == "ternary")}
    return None


def records(run):
    data = json.loads(run.read_text(encoding="utf-8"))
    if isinstance(data, dict):
        return [(path, rec) for path, rec in data.items()]
    return [(rec["path"], rec) for rec in data if isinstance(rec, dict) and "path" in rec]


def tags_for(library, labels, path):
    track = library.get(path, {})
    file_tags = track.get("tags") or {}
    dbz = track.get("dancebrainz") or {}
    lab = labels.get(path) or {}
    dances = [{"name": d.get("name"), "path": d.get("path"),
               "qualifier": d.get("qualifier")} for d in dbz.get("dances") or []]
    return {
        "mbid": file_tags.get("mbid") or lab.get("mbid"),
        "artist": file_tags.get("artist"),
        "album": file_tags.get("album"),
        "title": file_tags.get("title"),
        "date": file_tags.get("date"),
        "dance": dances[0]["name"] if dances else None,
        "dancebrainz": {
            "dances": dances,
            "meters": dbz.get("meters") or [],
            "structures": dbz.get("structures") or [],
            "bpm": dbz.get("bpm"),
            "raters": dbz.get("raters"),
        },
        "label": {k: lab[k] for k in ("meter", "type", "status", "source") if k in lab} or None,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--runs-dir", type=Path, default=Path("~/folksentia-cloud/meter-runs").expanduser())
    ap.add_argument("--library", type=Path, default=Path("~/folksentia-cloud/library.json").expanduser())
    ap.add_argument("--labels", type=Path, default=HERE / "meter_labels.json")
    ap.add_argument("-o", "--output", type=Path, default=HERE / "measurements.jsonl")
    ap.add_argument("--long", action="store_true", help="one row per tune and run")
    args = ap.parse_args()

    library = json.loads(args.library.read_text(encoding="utf-8"))["tracks"]
    labels = {t["file"]: t for t in
              json.loads(args.labels.read_text(encoding="utf-8"))["tunes"]}
    rows = {}
    for run in sorted(args.runs_dir.glob("*.json")):
        source = run.stem
        for path, rec in records(run):
            measures = {k: v for k, v in rec.items() if k not in ("path", "dance")}
            row = rows.setdefault(path, {"path": path, **tags_for(library, labels, path), "runs": {}})
            row["runs"][source] = {"dance": rec.get("dance"), **measures}

    for row in rows.values():
        row["finding"] = finding_for(row)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as out:
        for path in sorted(rows):
            row = rows[path]
            if args.long:
                for source, measures in sorted(row["runs"].items()):
                    long_row = {k: v for k, v in row.items() if k != "runs"}
                    long_row.update(source=source, measures=measures)
                    out.write(json.dumps(long_row, ensure_ascii=False, separators=(",", ":")) + "\n")
            else:
                out.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
    sources = sorted({s for r in rows.values() for s in r["runs"]})
    print(f"{len(rows)} tunes, {sum(len(r['runs']) for r in rows.values())} measurements, "
          f"{len(sources)} runs ({', '.join(sources)}) -> {args.output}")
    print(f"library: {len(library)} tracks; labels: {len(labels)}; "
          f"{sum(not r['label'] for r in rows.values())} tunes without a meter label; "
          f"{sum(bool(r['finding']) for r in rows.values())} with a phrase-lag finding")


if __name__ == "__main__":
    main()
