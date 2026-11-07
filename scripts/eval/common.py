"""Shared paths and helpers for the folksentia evaluation scripts (stdlib only)."""

import json
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO / "builddir" / "folksentia"


def data_dir() -> Path:
    """Root for downloaded datasets, generated signals and results.

    Override with FOLKSENTIA_EVAL_DATA. Kept outside the repo because the
    Ballroom audio is copyrighted and must not be committed.
    """
    return Path(os.environ.get("FOLKSENTIA_EVAL_DATA",
                               Path.home() / "data" / "folksentia-eval"))


def results_dir(dataset: str, variant: str) -> Path:
    return data_dir() / "results" / dataset / variant


def load_results(dataset: str, variant: str) -> dict:
    """Map file stem -> folksentia result object for one run."""
    out = {}
    d = results_dir(dataset, variant)
    if not d.is_dir():
        raise SystemExit(f"no results in {d}; run run.py first")
    for p in sorted(d.glob("*.json")):
        if p.name == "meta.json":
            continue
        out[p.stem] = json.loads(p.read_text())
    return out
