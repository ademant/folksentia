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
| `-w`, `--waveform[=BINS]` | include `waveform` with BINS peaks (default 150) |
| `-p`, `--fingerprint` | include `fingerprint`, a Chromaprint fingerprint of the first 120 s |
| `-a`, `--array` | print a JSON array even when only one file is given |
| `-f`, `--fast` | use the faster `degara` beat tracker instead of `multifeature` (`bpm_confidence` is then always 0) |
| `-r`, `--sample-rate HZ` | analysis sample rate: 8000, 16000, 32000, 44100 or 48000 (default 44100); tempo and beats are always analysed at 44100 Hz |

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
| `bpm`, `bpm_confidence` | number | tempo and its confidence (0 with `--fast`) |
| `key`, `scale` | string | key and mode, `null` if unknown |
| `key_strength` | number | key detection strength |
| `tuning_frequency`, `tuning_cents` | number | reference A in Hz and deviation from 440 Hz |
| `replay_gain_db` | number | ReplayGain |
| `loudness_db`, `dynamic_complexity` | number | Essentia's dynamic complexity and loudness |
| `silence` | object | see below |
| `loudness` | object | see below |
| `waveform` | object | `--waveform` only, see below |
| `beats` | array of seconds | `--beats` or `--downbeats` only |
| `downbeatIndexes` | array of ints or `null` | `--downbeats` only; indexes into `beats` that start a bar |
| `fingerprint` | object or `null` | `--fingerprint` only, see below |

Times in seconds (`duration`, `beats`, `fingerprint.durationSec`) are printed
with three decimals, i.e. to the millisecond, however long the file is.

`silence` (`schema` 1):

- `leadMs`: time before the first non-silent frame, in milliseconds.
- `tailMs`: time after the last non-silent frame, in milliseconds.

Frames are 512 samples long and advance by 256 samples, so each value is
accurate to about 6 ms at 44.1 kHz. Both are `null` if the whole file is silent.

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
structure, for example with fewer than 8 beats or when no bar phase is clearly
louder than the others. It assumes 4/4 time. The bass (below 150 Hz) energy
just after each beat is compared across the four possible bar phases. The
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

## License

Essentia is AGPL-3.0; a binary statically linking it is therefore AGPL-3.0 as well.
folksentia itself is licensed under AGPL-3.0-or-later; see `LICENSE`.
