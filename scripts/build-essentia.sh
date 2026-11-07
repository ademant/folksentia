#!/usr/bin/env bash
# Build the Essentia submodule as a static library and install it into
# ./build/essentia-prefix, where meson.build picks it up via pkg-config.
#
#   JOBS=2 scripts/build-essentia.sh      # fewer jobs = less RAM
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/third_party/essentia"
PREFIX="${ESSENTIA_PREFIX:-$ROOT/build/essentia-prefix}"
JOBS="${JOBS:-2}"

if [[ ! -f "$SRC/waf" ]]; then
  git -C "$ROOT" submodule update --init --recursive third_party/essentia
fi

cd "$SRC"
python3 waf configure \
  --prefix="$PREFIX" \
  --build-static \
  --mode=release \
  --lightweight=libav,libsamplerate,fftw,libchromaprint \
  --fft=FFTW
python3 waf build -j"$JOBS"
python3 waf install

echo
echo "Essentia installed to $PREFIX"
