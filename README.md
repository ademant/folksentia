# folksentia

A command-line audio feature extractor built on [Essentia](https://github.com/MTG/essentia).
For each input file it prints JSON with duration, tempo (BPM + confidence),
key/scale, tuning frequency, ReplayGain, loudness, dynamic complexity, silence,
EBU R128 loudness and true peak, and optionally beats, downbeats, waveform peaks
and a Chromaprint audio fingerprint.

## Build

Essentia is vendored as a git submodule (`third_party/essentia`) and linked
statically. The Essentia version is pinned by the submodule to commit
`33108c70929684ed14091dfb671a30264cf8bb84`. That commit needs FFmpeg 7.1 or
newer (Debian 13 / Raspberry Pi OS trixie, Ubuntu 25.04 or later); Ubuntu 24.04
ships FFmpeg 6.1, and Essentia fails to build there.

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
| `-s`, `--silence-threshold DB\|auto` | frame power in dBFS below which a frame counts as silence, −150 to 0 (default −100); `auto` sets it from the track's noise floor and loudness (see below) |
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
| `key`, `scale` | string or `null` | key and mode, measured against the file's own tuning; `null` if unknown or weak (see below) |
| `key_strength` | number or `null` | key detection strength, 0..1 |
| `tuning_frequency`, `tuning_cents` | number or `null` | reference A in Hz and deviation from 440 Hz |
| `tuning_spread_cents` | number or `null` | how much the tuning measured per 30 s segment varies (see below); `null` with fewer than two segments |
| `replay_gain_db` | number | ReplayGain |
| `loudness_db`, `dynamic_complexity` | number | Essentia's dynamic complexity and loudness |
| `spectral_complexity`, `spectral_entropy` | number or `null` | how many frequencies the sound uses (see below); `null` for digital silence |
| `silence` | object | see below |
| `loudness` | object | see below |
| `waveform` | object | `--waveform` only, see below |
| `beats` | array of seconds | `--beats` or `--downbeats` only |
| `downbeatIndexes` | array of ints or `null` | `--downbeats` only; indexes into `beats` that start a bar |
| `fingerprint` | object or `null` | `--fingerprint` only, see below |
| `rhythm` | object or `null` | pulse, bar, accents and tempo curve (experimental), see below |

**Spectral complexity and entropy.** Two measures of how many frequencies a
recording uses, each the median over all non-silent frames (2048 samples, hop
1024, Blackman-Harris window, as in Essentia's MusicExtractor, so the values
compare with its `spectral_complexity` and `spectral_entropy`).
`spectral_complexity` is the number of spectral peaks per frame: 2 for a tone
with two partials, 59 for white noise; six folk dance tracks read 9 to 20.
`spectral_entropy` is the Shannon entropy of the magnitude spectrum in bits:
low when the energy sits in a few tones (3 for the two-partial tone), high when
it is spread out like noise or percussion (8.7 for white noise; at most
log2(1025) = 10); the same six tracks read 6.9 to 7.8.

**Key and tuning.** The key is detected against the measured tuning, not
against A = 440 Hz. `key` and `scale` are `null` when `key_strength` is below
0.6; `key_strength` is still reported. On folk recordings, reliable keys came
out at 0.85–0.95, and unaccompanied singing that drifted by a quarter tone at
0.41–0.54.

`tuning_spread_cents` is the range (10th to 90th percentile) of the tuning
measured in each 30-second segment, relative to the overall tuning and wrapped
to ±50 cents. It is a confidence measure for `tuning_frequency`: a large value
means either that the pitch really moves (singers drifting, retuning) or that
the estimate is unstable (dense, heavily limited mixes). Measured values: 4–12
cents for most recordings, 18–24 for some, and about 68 both for an a cappella
song and for a loudly mastered band recording.

For digital silence (no frame above −100 dBFS) tempo, key and tuning are not
measured: `bpm`, `bpm_confidence`, `key`, `scale`, `key_strength`,
`tuning_frequency`, `tuning_cents` and `tuning_spread_cents` are `null` and `beats` is empty.

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

`--silence-threshold auto` sets the threshold per track: 6 dB above
`noiseFloorLufs`, or 40 LU below `integratedLufs` if that is higher, but always
at least 20 LU below `integratedLufs`, so quiet music is not cut. Loudness is
K-weighted and sums both channels while the silence frames are measured on the
mono mix, so the threshold is converted with a fixed offset (−2.3 dB); it is an
approximation. `thresholdDb` reports the threshold used. Digital silence has no
loudness; the threshold then stays at −100 dB.

`loudness` (`schema` 1):

- `integratedLufs`: EBU R128 integrated loudness (LUFS), from Essentia's `LoudnessEBUR128`.
- `truePeakDbtp`: true peak in dBTP, the highest peak of either channel, measured as ITU-R BS.1770-4 specifies: 4x oversampling by a polyphase FIR interpolator (49-tap Hann-windowed sinc, as in libebur128).
- `lraLu`: loudness range (EBU Tech 3342) in LU, from Essentia's `LoudnessEBUR128`: the spread between the 10th and 95th percentile of the 3 s short-term loudness, ignoring passages more than 20 LU below its mean. Roughly 3–6 LU for a compressed master, 10–15 LU for a lively acoustic recording, 20 LU and more for classical music or field recordings.
- `plrDb`: peak-to-loudness ratio, `truePeakDbtp − integratedLufs`; small for a heavily limited master.
- `noiseFloorLufs`: the 2nd percentile of the momentary loudness (400 ms blocks every 100 ms), digital silence left out: the level of hiss or room tone, or of the quietest passages when the recording has no pauses.

The loudness values are measured at the file's own sample rate. A mono
file is measured as dual mono (the same signal on both channels). All values are
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

### Rhythm (experimental)

`bpm` comes from Essentia's beat tracker, which counts different note values
from tune to tune: on folk dance recordings it counted dotted quarters in one
hanter dro, half notes in two others, and an even pulse that drifts through an
11/8 bar. `rhythm` measures the music's own cycle instead. `null` for digital
silence, files shorter than 10 s, or when no cycle can be measured.

```json
"rhythm": {
  "schema": 1,
  "pulseSec": 0.527, "barPulses": 3, "barSec": 1.581, "barsPerMin": 38,
  "barScore": 0.34,
  "alternatives": [{"pulses": 6, "sec": 3.161, "score": 0.23}, {"pulses": 2, "sec": 1.054, "score": 0.11}],
  "accents": {"energy": [1, 0.57, 0.41], "bass": [0.73, 1, 0.87]},
  "tempoCurve": [{"startSec": 0.0, "barsPerMin": 38}, {"startSec": 30.0, "barsPerMin": 37.9}],
  "tempoStability": {"jitterPct": 1.22, "driftPct": 2.92, "slopePctPerMin": 0.38},
  "microtiming": {"npvi": 32.7, "withinBarPct": 5.6, "acrossBarsPct": 2.7, "beatsPerBar": 2,
                  "perBeat": 2, "bars": 225, "profile": [0, 0.207, 0.507, 0.688]}
}
```

- `pulseSec`: the beat-level pulse, between 0.1 and 0.6 s: the period whose
  multiples line up best with the autocorrelation of the onset envelope.
- `barPulses`, `barSec`, `barsPerMin`: the bar as a whole number of pulses
  (between 0.8 and 4.5 s). It is the cycle with clearly the most repeating
  structure of its own (structure that is not periodic with a shorter cycle),
  or, when no cycle stands out from the others and from the noise, the cycle
  that recurs most strongly. `barsPerMin` is what dancers count; it was
  29–31 for all three labelled hanter dros, whose `bpm` ranged from 86 to 125.
- `barScore`: the onset autocorrelation at the bar (0..1); `alternatives`: the
  next two cycles with their scores, for example two bars, or the half bar.
- `accents`: mean energy and bass (below 150 Hz) energy per pulse of the bar,
  relative to the strongest, starting with the strongest energy pulse (the
  bar start itself is not known). A valse showed one heavy beat
  (`[1, 0.57, 0.41]`), two mazurkas almost even beats (`[1, 0.86, 0.91]`,
  `[1, 0.85, 0.87]`).
- `tempoCurve`: bars per minute per 30-second window (one point for shorter
  files), which shows accelerandos. Each window's bar is searched within
  ±5 % of the whole track's, so larger changes show in `tempoStability`.
- `tempoStability`: how steady the tempo is; each value is `null` when there
  is too little to measure.
  - `jitterPct`: beat-to-beat irregularity: per 8 beat intervals the mean
    absolute deviation from their median, in % of it; the median over the
    track. The beat times are quantised to 11.6 ms, so about 0.3–1 % is the
    floor (steady clicks read 0.3). `null` with fewer than 20 beat intervals.
  - `driftPct`: how far the bar tempo moves over the track, the 10th to 90th
    percentile range in % of the median. The tempo is measured in windows of
    4 bars (at least 8 s), and the path through the windows is chosen to
    follow lasting changes but not single noisy windows; changes between 0.8
    and 1.25 times the track's tempo are seen.
  - `slopePctPerMin`: the trend of that tempo, in % per minute; positive for
    an accelerando (clicks from 100 to 125 BPM in a minute read 22).
  - `driftPct` and `slopePctPerMin` are `null` when `barScore` is below 0.1:
    without a clear bar there is no tempo to follow (rubato, free singing).
  On 140 tracks of a folk dance library, the median jitter was 1.2 %, drift
  2.9 % (90th percentile 12 %) and slope 0 %/min.
- `microtiming`: how the notes sit on the bar, or `null` when there are fewer
  than 8 usable bars or too few onsets. Note onsets (spectral flux, 5.8 ms
  hop) are placed in bars of `beatsPerBar` beat ticks (the whole number of
  beats closest to the rhythm's bar, 1 when there is none; the bar starts on
  the tick phase with the strongest onsets). Bars in which the beat tracker
  switches note values are left out; `bars` is the number used. Each beat is
  divided into `perBeat` = 2, 3 or 4 grid slots, whichever the onsets fit
  best.
  - `withinBarPct`: how unevenly the bar is played: the mean offset of each
    slot's notes from the even grid, relative to the bar's first slot,
    averaged over the notes, in % of a beat. 60:40 eighth notes read 3.4,
    straight ones 0.
  - `acrossBarsPct`: how much the timing varies from bar to bar: per slot the
    median absolute deviation of its notes' offsets, averaged over the notes,
    in % of a beat. A band that repeats an uneven pattern exactly reads low;
    straight clicks read 0.4.
  - `profile`: the slots' mean positions in the bar (fraction of the bar,
    first slot at 0): the lilt itself, e.g. `[0, 0.207, 0.507, 0.688]` for a
    bourrée whose first eighth of each beat is long.
  - `npvi`: normalized pairwise variability index of successive inter-onset
    intervals up to two beats, `100 / (m − 1) · Σ |d_k − d_{k+1}| / ((d_k + d_{k+1}) / 2)`:
    0 for equal note lengths, higher the more neighbouring notes differ
    (intended variety and timing errors together).
  On 54 dance-tagged tracks, mazurkas varied most from bar to bar (8–10 %),
  straight polkas and an dros least (1–4 %); some bourrées and valses showed a
  clear lilt (`withinBarPct` 4–6). Legato instruments (accordion, voice) give
  fewer onsets, and `beatsPerBar` follows the rhythm's bar, which is sometimes
  two bars.

On the labelled tunes in `scripts/eval/folk_labels.json`, the bar is right for
6 of 7 (three hanter dros, a valse à 11, a valse and two mazurkas); an a
cappella hanter dro gets 4 quarters instead of 6. These tunes were also used
to design the method, so expect lower accuracy on other music: check
`barScore` and `alternatives`. The grouping of the bar into beats (4+2,
3+3+2+3) is not reported; it could not be measured reliably.

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
file are skipped, and a half-written last line is removed. Ctrl-C, SIGTERM and
SIGHUP stop the running folksentia processes too (they run in their own
sessions); a run started in the background ignores Ctrl-C, so stop it with
`kill`. Exit status: 0 when every job has a line without an error, 1 when some
analysis failed, 2 for bad usage or an unreadable job list, 128 + the signal
number when stopped.

## License

Essentia is AGPL-3.0; a binary statically linking it is therefore AGPL-3.0 as well.
folksentia itself is licensed under AGPL-3.0-or-later; see `LICENSE`.
