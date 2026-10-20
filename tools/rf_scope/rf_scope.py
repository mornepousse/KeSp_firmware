#!/usr/bin/env python3
"""rf_scope — see both ends of the KeSp radio link. See README.md."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rftrace.cli import main, parse_duration, analyze_file, render_dump  # noqa: E402,F401

if __name__ == "__main__":
    sys.exit(main())
