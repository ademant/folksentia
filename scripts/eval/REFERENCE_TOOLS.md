# Reference tools in Debian

Other tools packaged in Debian/Ubuntu that compute some of the values
folksentia outputs. They are useful as references when there is no ground
truth for a file: if folksentia and an independent tool agree, the value is
probably right; if they disagree, look closer.

Availability was checked against Ubuntu "resolute" (Debian trixie/sid ship
nearly the same set). Version numbers are from that check.

## Independence matters

folksentia is built on Essentia. Anything else built on Essentia
(`python3-essentia`, the `essentia_streaming_extractor_music` binaries,
AcousticBrainz data) only shows that folksentia calls Essentia correctly. It
says nothing about whether Essentia's algorithms are right. The tools below
use their own implementations, except where noted.

## Overview

| folksentia field | reference tool | package | agreement to expect |
|---|---|---|---|
| `fingerprint` | `fpcalc` | `libchromaprint-tools` (1.6.0) | identical or almost identical (same library) |
| `loudness.integratedLufs` | `ffmpeg -af ebur128` | `ffmpeg` (8.0) | ±0.1 LU |
| `loudness.integratedLufs` | `bs1770gain`, `libebur128` | `bs1770gain` (0.9.8), `libebur128-1` | ±0.1 LU |
| `loudness.truePeakDbtp` | `ffmpeg -af ebur128=peak=true`, `bs1770gain --truepeak` | as above | a few tenths of a dB (oversampling differs) |
| `replay_gain_db` | `rsgain`, `loudgain` | `rsgain` (3.6), `loudgain` (0.6.8) | constant offset, not equality (see below) |
| `bpm` | `aubio tempo` | `aubio-tools` (0.4.9) | same value or ×2 / ×½ |
| `bpm` | `bpm` | `bpm-tools` (0.3) | rough check only |
| `bpm` | `soundstretch -bpm` | `soundstretch` (2.4.0) | rough check only |
| `beats` | `aubio beat` | `aubio-tools` | score with `mir_eval.beat` |
| `beats` | `vamp-simple-host` + example plugins | `vamp-examples` (2.10) | onsets and a fixed-tempo estimate only |
| `key`, `scale` | Mixxx key analysis (libkeyfinder / Queen Mary) | `mixxx` (2.5.4) | same key, or relative major/minor, or fifth |
| `bpm` | Mixxx beat analysis (Queen Mary) | `mixxx` | same value or ×2 / ×½ |
| `silence.leadMs`, `silence.tailMs` | `ffmpeg -af silencedetect`, `sox ... silence` | `ffmpeg`, `sox` (14.7) | depends on threshold and minimum duration |
| `duration` | `ffprobe`, `soxi` | `ffmpeg`, `sox` | exact up to codec padding (MP3/AAC priming) |
| `waveform` | `ffmpeg showwavespic` / `astats`, `sox` | `ffmpeg`, `sox` | visual comparison only |

No Debian reference exists for `tuning_frequency` / `tuning_cents`,
`dynamic_complexity`, `key_strength`, `bpm_confidence`, `downbeatIndexes` or
the `rhythm` object.

## Running all tools with one decode

```sh
python3 reference.py ~/Musik/*.opus > reference.json
```

`reference.py` decodes each file once with ffmpeg. An `asplit` filter sends
one copy to ffmpeg's own analysers (`ebur128`, `silencedetect`, `astats`) and
one copy per reference tool to a separate ffmpeg output. Each output has the
format that tool expects (raw float for `bpm`, raw s16 for `fpcalc`, WAV for
`aubio` and `soundstretch`) and is written to a pipe (`pipe:N`) that is the
tool's stdin. All tools run in parallel. The exception is `soundstretch`: it
rewinds its input after the BPM pass and asserts on a pipe, so ffmpeg writes
its WAV to a temporary file and `soundstretch` runs on it once ffmpeg is done.
Tools that are not installed are listed under `skipped`.

This is better than `tee`: with `tee` every consumer gets the same byte
stream, but the tools need different formats, and one consumer that stops
reading early (fpcalc after 120 s) breaks the pipe for all of them. Here
ffmpeg converts per output and caps the fingerprint stream with `-t 120`.

`rsgain`, `loudgain`, `bs1770gain` and Mixxx only read files, so they are not
part of the single decode. ReplayGain 2.0 is computed from ffmpeg's
integrated loudness instead (`replayGain2Db` = −18 − LUFS), which is what
rsgain and loudgain do.

ffmpeg prints the `ebur128` summary with one decimal, so loudness and true
peak are only compared to 0.1 dB.

## Per metric

### Chromaprint fingerprint

```sh
fpcalc -length 120 -raw tune.flac
```

folksentia fingerprints the first 120 s with the same libchromaprint, so the
raw integer arrays should match. Small differences at the end can come from
resampling or decoder differences; compare with the AcoustID similarity
(bit error rate over aligned integers) rather than string equality.

### EBU R128 loudness and true peak

```sh
ffmpeg -nostats -i tune.flac -af ebur128=peak=true -f null - 2>&1 | tail -12
bs1770gain --integrated --truepeak tune.flac
```

These are independent of Essentia and widely used; `ffmpeg` is the de-facto
reference. The EBU Tech 3341 check in this directory (`check_ebu.py`) already
uses signals verified against ffmpeg. For real recordings, integrated loudness
should agree within ±0.1 LU. True peak can differ by a few tenths of a dB
because each tool oversamples differently (Tech 3341 allows +0.2 / −0.4 dB).

### ReplayGain

```sh
rsgain custom -O tune.flac       # tab-separated output, no tags written
loudgain -O tune.flac
```

`rsgain` and `loudgain` implement ReplayGain 2.0: gain = −18 LUFS minus the
EBU R128 integrated loudness. Essentia's `ReplayGain` is the older
ReplayGain 1 algorithm (equal-loudness filter, 95th percentile of RMS
frames, reference 89 dB SPL). The two will not be equal. Across many files the
difference should be roughly constant; large outliers point to a problem in
one of them.

### Tempo

```sh
aubio tempo tune.flac
sox tune.flac -t raw -r 44100 -e float -b 32 -c 1 - | bpm
soundstretch tune.wav -bpm        # a WAV file; not stdin (it rewinds its input)
```

`aubio` uses its own onset and autocorrelation method, so it is a good
independent check. Octave errors (×2, ×½, and ×3 / ×⅓ for triple meters) are
common in every tracker; count them separately, as `score_ballroom.py` does
with Acc2. `bpm-tools` and `soundstretch` are simpler and only catch gross
errors.

### Beats and onsets

```sh
aubio beat tune.flac  > aubio_beats.txt
aubio onset tune.flac > aubio_onsets.txt
vamp-simple-host vamp-example-plugins:percussiononsets tune.wav
vamp-simple-host vamp-example-plugins:fixedtempo tune.wav
```

Compare `aubio beat` against folksentia's `beats` with `mir_eval.beat.f_measure`
(±70 ms) and the continuity metrics, the same way `score_ballroom.py` compares
against annotations. Without ground truth, agreement only means both trackers
chose the same metrical level; it does not say which level is right.

The Queen Mary, BBC and NNLS-Chroma Vamp plugins and `sonic-annotator` are not
packaged; only the SDK example plugins are.

### Key

Mixxx is the only packaged tool with a key detector, and it has no CLI.
Import the files into its library, run "Analyze", then read the results:

```sh
sqlite3 ~/.mixxx/mixxxdb.sqlite \
  "SELECT l.location, t.key, t.bpm FROM library t JOIN track_locations l ON t.location = l.id;"
```

Mixxx uses libkeyfinder (default) or the Queen Mary key detector, both
independent of Essentia. Use the MIREX weighting when comparing: same key 1.0,
fifth 0.5, relative major/minor 0.3, parallel 0.2. Modal folk tunes (dorian,
mixolydian) often land on a neighbouring key in any major/minor detector.

### Silence at start and end

```sh
ffmpeg -nostats -i tune.flac -af silencedetect=noise=-100dB:d=0.01 -f null - 2>&1 | grep silence_
```

ffmpeg reports silent stretches with a level threshold and a minimum
duration; folksentia counts frames whose power is below `--silence-threshold`.
Set the same threshold in dBFS and a short minimum duration, then compare the
first `silence_end` with `leadMs` and the last `silence_start` with
`duration − tailMs`. Expect differences of up to one analysis frame.

### Duration

```sh
ffprobe -v error -show_entries format=duration -of csv=p=0 tune.mp3
soxi -D tune.flac
```

Lossless formats should match to the sample. For MP3 and AAC, encoder delay
and padding can shift the result by some tens of milliseconds depending on
whether the decoder trims them.

## Not in Debian, but worth knowing

- **madmom** (`pip install madmom`): RNN + DBN beat and downbeat tracker,
  the usual baseline in MIR papers. The best available reference for
  `beats` and `downbeatIndexes`.
- **librosa** (`pip install librosa`): beat tracking, tempo, chroma; another
  independent baseline.
- **keyfinder-cli**: CLI around libkeyfinder (the library Mixxx uses); has to
  be built from source.
- **sonic-annotator** with the Queen Mary Vamp plugins: batch key, tempo,
  beats and onsets; built from source.
