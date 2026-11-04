# folksentia

A command-line audio feature extractor built on [Essentia](https://github.com/MTG/essentia).
For each input file it prints JSON with duration, tempo (BPM + confidence),
key/scale, tuning frequency, ReplayGain, loudness and dynamic complexity.

## Build

Essentia is vendored as a git submodule (`third_party/essentia`) and linked
statically.

```sh
# 1. system dependencies (Debian/Ubuntu/Raspberry Pi OS)
sudo apt install build-essential meson ninja-build pkg-config python3 \
  libeigen3-dev libfftw3-dev libsamplerate0-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev

# 2. fetch + build Essentia (slow the first time; JOBS=1 on low-RAM machines)
git submodule update --init
JOBS=2 scripts/build-essentia.sh

# 3. build folksentia
meson setup builddir
meson compile -C builddir
```

## Usage

```sh
builddir/folksentia tune.mp3
builddir/folksentia --fast --beats *.flac > features.json
```

| option | meaning |
|---|---|
| `-b`, `--beats` | include beat positions (seconds) |
| `-f`, `--fast` | use the faster `degara` beat tracker instead of `multifeature` (`bpm_confidence` is then always 0) |
| `-r`, `--sample-rate HZ` | analysis sample rate (default 44100) |

## License

Essentia is AGPL-3.0; a binary statically linking it is therefore AGPL-3.0 as well.
