#!/usr/bin/env python3
"""Compatibility entry point for the truthful bounded lab runner."""

import os
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(BASE_DIR, "scripts", "lab", "run_iteration.py")
os.execv(sys.executable, [sys.executable, RUNNER, *sys.argv[1:]])
