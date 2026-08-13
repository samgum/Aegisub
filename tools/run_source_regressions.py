#!/usr/bin/env python3
"""Run every lightweight source-tree regression check with one command."""

from pathlib import Path
import subprocess
import sys


TOOLS = Path(__file__).resolve().parent


def main() -> int:
    scripts = sorted(TOOLS.glob("test_*.py"))
    if not scripts:
        raise RuntimeError("no source regression scripts found")

    for script in scripts:
        print(f"==> {script.name}", flush=True)
        subprocess.run([sys.executable, str(script)], check=True)

    print(f"All {len(scripts)} source regression scripts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
