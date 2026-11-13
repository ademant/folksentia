#!/usr/bin/env python3
"""Print the version info for src/version.h.in, as two C string literals:

  "0.1.0+g125f36d", "2026-11-13"

The version is meson's project version plus the git commit (".dirty" for
uncommitted changes) when building from a git checkout. The build date is
today in UTC, or SOURCE_DATE_EPOCH when set (reproducible builds).

Usage: version.py PROJECT_VERSION SOURCE_DIR
"""

import datetime
import os
import subprocess
import sys

version, source = sys.argv[1], sys.argv[2]


def git(*args):
    return subprocess.run(["git", "-C", source, *args], capture_output=True, text=True, check=True).stdout.strip()


try:
    sha = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=dirty")
    version += f"+g{sha}" + (".dirty" if dirty else "")
except (OSError, subprocess.CalledProcessError):
    pass  # not a git checkout: the plain version

epoch = os.environ.get("SOURCE_DATE_EPOCH")
when = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) if epoch else datetime.datetime.now(datetime.timezone.utc)
print(f'"{version}", "{when:%Y-%m-%d}"')
