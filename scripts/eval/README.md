# folksentia evaluation

Scripts that check folksentia against public reference data with ground truth,
so that changes and parameter choices can be measured instead of guessed.

| dataset | checks | needs |
|---|---|---|
| EBU Tech 3341 signals (synthesised) | integrated loudness ±0.1 LU, true peak +0.2/−0.4 dB, plus documented edge cases (mono, 44.1 kHz, silence) | stdlib only |
| [Ballroom](https://mtg.upf.edu/ismir2004/contest/tempoContest/node5.html) (698 × 30 s, 10 dance styles) | tempo, beats, downbeats | numpy, mir_eval |

Data and results live outside the repo in `$FOLKSENTIA_EVAL_DATA`
(default `~/data/folksentia-eval`). The Ballroom audio is copyrighted: never
commit it. Commit only scripts and summary tables.

## Setup

```sh
cd scripts/eval
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt  # for score_ballroom.py
python3 make_ebu_signals.py      # ~100 MB, a few seconds
python3 fetch_ballroom.py        # streams 1.45 GB, unpacks ~1.5 GB
```

The official EBU loudness test set cannot be fetched by script (the EBU site
refuses non-browser requests). The cases checked here are fully specified in
Tech 3341, so `make_ebu_signals.py` synthesises them; the signals were checked
against ffmpeg's `ebur128` filter. To check the official files as well, download
the zip from <https://tech.ebu.ch/publications/ebu_loudness_test_set> by hand
and unpack it into `$FOLKSENTIA_EVAL_DATA/ebu/official/`.

## Run and score

```sh
python3 run.py ebu && python3 check_ebu.py          # exits 1 on any failure

python3 run.py ballroom -j 2                        # variant "default", ~40 min on 2 cores
python3 run.py ballroom --variant fast -j 2 -- --fast
.venv/bin/python score_ballroom.py default fast
.venv/bin/python score_ballroom.py default --csv /tmp/per-file.csv
```

`run.py` stores one JSON per file under `results/<dataset>/<variant>/` and
skips files already analysed, so interrupted runs resume. Arguments after
`--` are passed to folksentia; a variant name is tied to its flags (see
`meta.json`). Use `--per-style N` for a quick subset.

After changing folksentia, delete the variant's results directory (or use a
new variant name) and run again.

## Ballroom metrics

All in percent, averaged per dance style and overall:

- **Acc1 / Acc2**: tempo within 4 % of the reference; Acc2 also accepts ×2, ×3,
  ×½, ×⅓. `x2` / `x½` show how often the estimate is double or half the reference.
- **BeatF, CMLt, AMLt**: standard `mir_eval` beat metrics (±70 ms, first 5 s
  ignored). AMLt also accepts double/half tempo and off-beat phase.
- **DbF**: downbeat F-measure (±70 ms). **Db?**: share of files where
  folksentia gave `downbeatIndexes: null`.
- **meter**: beats per bar in the annotations. Waltz and Viennese waltz are
  3/4; folksentia's downbeat heuristic assumes 4/4.
