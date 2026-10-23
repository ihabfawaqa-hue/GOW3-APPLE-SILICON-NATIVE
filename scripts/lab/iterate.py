#!/usr/bin/env python3
"""Entry point for the bounded local GOW3 test laboratory."""

from pathlib import Path
import os
import sys

RUNNER = Path(__file__).with_name("run_iteration.py")
os.execv(sys.executable, [sys.executable, str(RUNNER), *sys.argv[1:]])
