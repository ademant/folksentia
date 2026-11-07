#!/usr/bin/env python3
"""Download the Ballroom dataset (audio + tempo + beat/downbeat annotations).

Layout written under $FOLKSENTIA_EVAL_DATA/ballroom/:
  audio/<Style>/<name>.wav    698 30-second excerpts (~1.5 GB)
  tempo/<name>.bpm            ISMIR 2004 tempo ground truth
  beats/<name>.beats          CPJKU beat annotations: "<seconds> <beat-in-bar>"

The 1.45 GB audio archive is streamed and unpacked on the fly, so the archive
itself is never stored on disk. Checksums are the ones mirdata uses.
"""

import argparse
import hashlib
import io
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

from common import data_dir

AUDIO = ("https://mtg.upf.edu/ismir2004/contest/tempoContest/data1.tar.gz",
         "2872a3e52070bc342a4510a95e2fa0b8")
TEMPO = ("https://mtg.upf.edu/ismir2004/contest/tempoContest/data2.tar.gz",
         "4a0ec5518bbb4dbf3ab02de0383b0994")
BEATS = ("https://github.com/CPJKU/BallroomAnnotations/archive/master.zip",
         "d0c31e1a30c0caf8fd22dec25f2174cf")


class HashingReader(io.RawIOBase):
    """Wraps a response, hashing everything read and reporting progress."""

    def __init__(self, resp, total):
        self.resp, self.total, self.done = resp, total, 0
        self.md5 = hashlib.md5()
        self._last = -1

    def readable(self):
        return True

    def readinto(self, b):
        chunk = self.resp.read(len(b))
        n = len(chunk)
        b[:n] = chunk
        self.md5.update(chunk)
        self.done += n
        if self.total:
            pct = self.done * 100 // self.total // 5 * 5
            if pct != self._last:
                self._last = pct
                print(f"  {pct:3d}% of {self.total / 1e9:.2f} GB",
                      file=sys.stderr, flush=True)
        return n


def safe_name(s: str) -> str:
    if not s or s in (".", "..") or "/" in s or "\\" in s or s.startswith("."):
        raise ValueError(f"unsafe name in archive: {s!r}")
    return s


def fetch_small(url, md5):
    print(f"downloading {url}", file=sys.stderr)
    data = urllib.request.urlopen(url, timeout=60).read()
    got = hashlib.md5(data).hexdigest()
    if got != md5:
        raise SystemExit(f"checksum mismatch for {url}: {got} != {md5}")
    return data


def fetch_tempo(root: Path):
    out = root / "tempo"
    out.mkdir(parents=True, exist_ok=True)
    data = fetch_small(*TEMPO)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf:
            if m.isfile() and m.name.endswith(".bpm"):
                name = safe_name(m.name.rsplit("/", 1)[-1])
                (out / name).write_bytes(tf.extractfile(m).read())


def fetch_beats(root: Path):
    out = root / "beats"
    out.mkdir(parents=True, exist_ok=True)
    data = fetch_small(*BEATS)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        for name in zf.namelist():
            if name.endswith(".beats"):
                (out / safe_name(name.rsplit("/", 1)[-1])).write_bytes(zf.read(name))


def fetch_audio(root: Path):
    url, md5 = AUDIO
    out = root / "audio"
    print(f"streaming {url}", file=sys.stderr)
    resp = urllib.request.urlopen(url, timeout=60)
    reader = HashingReader(resp, int(resp.headers.get("Content-Length") or 0))
    n = 0
    with tarfile.open(fileobj=io.BufferedReader(reader, 1 << 20), mode="r|gz") as tf:
        for m in tf:
            parts = m.name.split("/")
            if not (m.isfile() and m.name.endswith(".wav") and len(parts) >= 2):
                continue
            if parts[-1].startswith("._"):  # macOS resource forks
                continue
            style, name = safe_name(parts[-2]), safe_name(parts[-1])
            dest = out / style / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_suffix(".part")
            with tf.extractfile(m) as src, open(tmp, "wb") as dst:
                while chunk := src.read(1 << 20):
                    dst.write(chunk)
            tmp.rename(dest)
            n += 1
    # drain the rest so the checksum covers the whole archive
    while reader.read(1 << 20):
        pass
    got = reader.md5.hexdigest()
    if got != md5:
        raise SystemExit(f"checksum mismatch for audio archive: {got} != {md5}")
    print(f"  {n} wav files", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--annotations-only", action="store_true",
                    help="skip the 1.45 GB audio archive")
    args = ap.parse_args()

    root = data_dir() / "ballroom"
    root.mkdir(parents=True, exist_ok=True)
    fetch_tempo(root)
    fetch_beats(root)
    marker = root / "audio" / ".complete"
    if args.annotations_only:
        pass
    elif marker.exists():
        print("audio already present", file=sys.stderr)
    else:
        fetch_audio(root)
        marker.touch()
    print(f"Ballroom data in {root}", file=sys.stderr)


if __name__ == "__main__":
    main()
