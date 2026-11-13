# folksentia

A command-line audio feature extractor built on [Essentia](https://github.com/MTG/essentia).
For each input file it prints JSON with duration, tempo (BPM + confidence),
key/scale, tuning frequency, ReplayGain, loudness, dynamic complexity, silence,
EBU R128 loudness and true peak, and optionally beats, downbeats, waveform peaks
and a Chromaprint audio fingerprint.

## Build

Essentia is vendored as a git submodule (`third_party/essentia`) and linked
statically. The Essentia version is pinned by the submodule to commit
`33108c70929684ed14091dfb671a30264cf8bb84`.

```sh
# 1. system dependencies (Debian/Ubuntu/Raspberry Pi OS)
sudo apt install build-essential meson ninja-build pkg-config python3 \
  libeigen3-dev libfftw3-dev libsamplerate0-dev libchromaprint-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev

# 2. fetch + build Essentia (slow the first time; JOBS=1 on low-RAM machines)
git submodule update --init
JOBS=2 scripts/build-essentia.sh

# 3. build folksentia
meson setup builddir
meson compile -C builddir
```

`meson setup` checks the system packages from step 1. If any are missing, it
stops and prints the `apt install` command for the ones that are missing.

Essentia only includes its `Chromaprinter` algorithm when libchromaprint is
installed at the time `scripts/build-essentia.sh` runs. If you built Essentia
before installing `libchromaprint-dev`, run the script again; until then
`--fingerprint` exits with an error.

To run the smoke tests (they synthesise small WAV files, so no test data is
needed):

```sh
meson test -C builddir --print-errorlogs
```

Accuracy against public reference data is checked separately, see
[`scripts/eval`](scripts/eval/README.md).

## Usage

```sh
builddir/folksentia tune.mp3
builddir/folksentia --fast --beats *.flac > features.json
builddir/folksentia --downbeats --waveform=200 tune.flac
builddir/folksentia --fingerprint *.opus > fingerprints.json
```

| option | meaning |
|---|---|
| `-b`, `--beats` | include beat positions (seconds) as `beats` |
| `-d`, `--downbeats` | include `downbeatIndexes`; implies `--beats` |
| `-m`, `--meter N` | beats per bar for `--downbeats`, 2 to 12 (default 4) |
| `-w`, `--waveform[=BINS]` | include `waveform` with BINS peaks (default 150) |
| `-p`, `--fingerprint` | include `fingerprint`, a Chromaprint fingerprint of the first 120 s |
| `-a`, `--array` | print a JSON array even when only one file is given |
| `-s`, `--silence-threshold DB` | frame power in dBFS below which a frame counts as silence, −150 to 0 (default −100) |
| `-f`, `--fast` | use the faster `degara` beat tracker instead of `multifeature` (`bpm_confidence` is then always 0) |
| `-V`, `--version` | print the version, build date, Essentia version and whether Chromaprint is available, then exit |
| `-r`, `--sample-rate HZ` | analysis sample rate: 8000, 16000, 32000, 44100 or 48000 (default 44100); tempo and beats are always analysed at 44100 Hz |

`--version` prints, for example:

```
folksentia 0.1.0+g125f36d
built 2026-11-13
Essentia 2.1-beta6-dev (v2.1_beta5-1555-g33108c70)
Chromaprint yes
```

The first line is always `folksentia VERSION`. VERSION is the release from
`meson.build`, plus `+g<commit>` when built from a git checkout (`.dirty` with
uncommitted changes). The build date is UTC, or taken from `SOURCE_DATE_EPOCH`
when that is set, for reproducible builds.

Exit status is 0 when every file was analysed, 1 when any file failed (the
other files are still printed), 2 for bad usage.

## Output schema (version 1)

Output is one object per file, or a JSON array when several files are given.
Use `--array` to always get an array, so that the output type doesn't depend on
how many files a glob matched.
Every object has `"schema": 1`. Every field listed below is always present in
its object; a field whose value is unknown is `null`, never missing. The
exceptions are the fields that are only printed when their option is given:
`waveform` (`--waveform`), `beats` (`--beats` or `--downbeats`),
`downbeatIndexes` (`--downbeats`) and `fingerprint` (`--fingerprint`), and
`file_base64`, which is only printed for paths that are not valid UTF-8.

A file that cannot be decoded gives an error object in place of its analysis:

```json
{"schema": 1, "file": "broken.mp3", "error": "no audio decoded from broken.mp3"}
```

Analysis object fields:

| field | type | meaning |
|---|---|---|
| `file` | string | input path as given (see [File names](#file-names)) |
| `file_base64` | string | only for paths that are not valid UTF-8: the exact path bytes, base64 |
| `duration` | number | seconds |
| `bpm`, `bpm_confidence` | number or `null` | tempo and its confidence (confidence is 0 with `--fast`); `null` when no tempo was found |
| `key`, `scale` | string or `null` | key and mode, `null` if unknown |
| `key_strength` | number or `null` | key detection strength |
| `tuning_frequency`, `tuning_cents` | number or `null` | reference A in Hz and deviation from 440 Hz |
| `replay_gain_db` | number | ReplayGain |
| `loudness_db`, `dynamic_complexity` | number | Essentia's dynamic complexity and loudness |
| `silence` | object | see below |
| `loudness` | object | see below |
| `waveform` | object | `--waveform` only, see below |
| `beats` | array of seconds | `--beats` or `--downbeats` only |
| `downbeatIndexes` | array of ints or `null` | `--downbeats` only; indexes into `beats` that start a bar |
| `fingerprint` | object or `null` | `--fingerprint` only, see below |

For digital silence (no frame above −100 dBFS) tempo, key and tuning are not
measured: `bpm`, `bpm_confidence`, `key`, `scale`, `key_strength`,
`tuning_frequency` and `tuning_cents` are `null` and `beats` is empty.

Times in seconds (`duration`, `beats`, `fingerprint.durationSec`) are printed
with three decimals, i.e. to the millisecond, however long the file is.

`silence` (`schema` 1):

- `thresholdDb`: a frame is silent when its mean power is below this many dB
  relative to full scale (`--silence-threshold`, default −100).
- `leadMs`: time before the first non-silent frame, in milliseconds.
- `tailMs`: time after the last non-silent frame, in milliseconds.

Frames are 512 samples long and advance by 256 samples, so each value is
accurate to about 6 ms at 44.1 kHz. Both are `null` if the whole file is silent.

The default of −100 dB only treats digital silence as silent: tape hiss, room
tone or vinyl noise count as sound, so a field recording usually gets `leadMs`
near 0. To measure the quiet before a tune starts, pass a threshold above the
noise floor, e.g. `--silence-threshold -50`. A full-scale sine wave has a mean
power of −3 dB.

`loudness` (`schema` 1):

- `integratedLufs`: EBU R128 integrated loudness (LUFS), from Essentia's `LoudnessEBUR128`.
- `truePeakDbtp`: true peak in dBTP, the highest peak of either channel, from `TruePeakDetector` (4x oversampling).

The loudness and true peak are measured at the file's own sample rate. A mono
file is measured as dual mono (the same signal on both channels). Both values are
`null` when the value is undefined, for example for digital silence.

`waveform` (`schema` 1, `--waveform` only):

- `bins`: number of bins, as requested.
- `peaks`: array of `bins` integers 0..255. Each is the largest absolute sample in that bin, scaled so the loudest bin is 255.

`downbeatIndexes` is a heuristic and is `null` when there is no clear bar
structure, for example with fewer than two bars of beats or when no bar phase
is clearly louder than the others. It assumes `--meter` beats per bar (default
4, e.g. 3 for a waltz); it does not detect the meter. The bass (below 150 Hz)
energy just after each beat is compared across the possible bar phases. The
phase is chosen when its beats are at least 20% louder than the other beats.
Treat the threshold as untuned until it has been checked on known files.

Essentia's pinned commit does not provide a downbeat or bar-tracking algorithm,
so this is folksentia's own computation on top of the beat ticks.

`fingerprint` (`schema` 1, `--fingerprint` only):

- `algorithm`: always `"chromaprint"`.
- `version`: Chromaprint algorithm version, currently 2 (Chromaprint's default
  `TEST2`, the one `fpcalc` and AcoustID use). Only compare fingerprints with
  the same `algorithm` and `version`.
- `durationSec`: seconds of audio fingerprinted: the first 120 s, or the whole
  file if it is shorter. The 120 s are fixed (the `fpcalc` default) so that
  fingerprints from different runs compare.
- `value`: the compressed, base64-encoded fingerprint, the same encoding
  `fpcalc` prints and AcoustID accepts.

The fingerprint is computed from the mono analysis signal (at
`--sample-rate`; Chromaprint resamples internally). `fingerprint` is `null`
when none could be computed: for digital silence, or when Chromaprint returns
no fingerprint (e.g. for a file only a few seconds long).

### File names

JSON text must be UTF-8, but file names on Linux are arbitrary bytes. When a
path is not valid UTF-8, `file` shows it with each invalid byte replaced by
U+FFFD (`\ufffd`), and `file_base64` holds the exact path bytes (standard
base64 with padding), so the result can still be matched to the file:

```json
{"schema": 1, "file": "Dans_l\ufffdil.opus", "file_base64": "RGFuc19sxWlsLm9wdXM=", ...}
```

The same replacement applies to every other string, for example error
messages that contain the path.

## Offline analysis for folksonic

[folksonic](https://github.com/ademant/folksonic) can have a whole library
analysed on another machine, for example a desktop with more memory and a
copy of the music folder. The server exports a job list, `folksentia-jobs`
runs folksentia over it, and the server imports the results:

```sh
# on the server
tinysonic analysis export -c /etc/tinysonic/tinysonic.toml -all -out jobs.json

# on the desktop (folksentia-jobs is built and installed next to folksentia)
builddir/folksentia-jobs --jobs jobs.json --music-root ~/Musik --out results.jsonl -j 4

# on the server
tinysonic analysis import -c /etc/tinysonic/tinysonic.toml results.jsonl
```

| option | meaning |
|---|---|
| `--jobs FILE` | job list from `tinysonic analysis export` |
| `--music-root DIR` | local copy of the music folder; job paths are relative to it |
| `--out FILE` | result file, one JSON line per job; appended to |
| `-j`, `--parallel N` | folksentia processes at a time (default: half the CPUs) |
| `--ignore-mtime` | compare file sizes only, for copies that did not keep modification times (import with `-size-only` then) |
| `--timeout SEC` | give up on a track after SEC seconds (default: no limit) |
| `--folksentia PATH` | folksentia binary (default: the one next to the script, else `PATH`) |

Each job runs in its own `folksentia` process, with the flags in the job
list's `options`, so a crash or an out-of-memory kill only fails that track.
Memory grows with track length (about 1 GB for a 6-minute track), so lower
`-j` on machines with little RAM.

Every job gets one line:

- `{"job": …, "local": {"size", "mtimeMs"}, "toolVersion": "…", "result": {…}}`
  with folksentia's normal output, its `file` replaced by the job's relative
  path. A crash or timeout becomes a result with `error`, like any other
  analysis error, and folksonic records it as a failed run.
- `{"job": …, "skipped": "missing"}` when the file is not in the music root
  (or the job path is absolute or contains `..`).
- `{"job": …, "local": …, "skipped": "changed"}` when its size or modification
  time differs from the job's.

`toolVersion` is the version from `folksentia --version`. The run can be
stopped and started again with the same arguments: jobs already in the result
file are skipped, and a half-written last line is removed. Exit status: 0 when
every job has a line without an error, 1 when some analysis failed, 2 for bad
usage or an unreadable job list.

## License

Essentia is AGPL-3.0; a binary statically linking it is therefore AGPL-3.0 as well.
folksentia itself is licensed under AGPL-3.0-or-later; see `LICENSE`.
