"""Import helpers for the WiFi Dap host tests (WP-302).

The production scripts under ``scripts/`` are plain Python files (no package),
so tests load them via ``importlib`` by path.  Only the standard library is used.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

# Tests must stay side-effect free: importing the production scripts must not
# create scripts/__pycache__ (the runner also sets PYTHONDONTWRITEBYTECODE).
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
DOCS = ROOT / "docs"
PORT_PARTITIONS = ROOT / "partitions"


def load_script(name: str):
    """Load ``scripts/<name>.py`` as a module without executing ``__main__``."""
    path = SCRIPTS / f"{name}.py"
    if not path.is_file():
        raise FileNotFoundError(f"production script not found: {path}")
    module_name = f"host_tests_{name}"
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    # Register before exec: dataclasses (used by check_partitions.py) resolve
    # their module through sys.modules during class creation.
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def read_doc(name: str) -> str:
    path = DOCS / name
    if not path.is_file():
        raise FileNotFoundError(f"contract document not found: {path}")
    return path.read_text(encoding="utf-8")


def read_partition_csv(name: str) -> str:
    path = PORT_PARTITIONS / name
    if not path.is_file():
        raise FileNotFoundError(f"partition table not found: {path}")
    return path.read_text(encoding="utf-8")
