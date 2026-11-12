#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Fail when the documentation names a CMake option the build does not declare.

CMake accepts an unknown -D variable as an unused cache entry and silently leaves the real option at
its default, so a misspelled or removed option in a command example is a bug that no build shows. This
reads the options declared in CMakeLists.txt, cmake/*.cmake and the firmware build files and checks every AOAHID_* option that
README.md, CONTRIBUTING.md, docs/*.md and examples/README.md mention: in a -D argument, or in code
quotes when the name looks like an option. It also fails on a misspelled prefix.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

PREFIX = "AOAHID"
TYPO = "A" + "OHID_"
# Names in code quotes that are options (the other AOAHID_ names are constants, macros and functions).
OPTION_SHAPE = re.compile(
    PREFIX + r"_(?:BUILD_[A-Z_]+|WERROR|SANITIZE|TSAN|USE_FAKE_LIBUSB|CHECK_SERIALIZATION|TINYUSB_PATH)\b"
)
DECLARED = re.compile(
    r"^\s*(?:option|set)\(\s*(" + PREFIX + r"_[A-Z0-9_]+)|\bDEFINED\s+(" + PREFIX + r"_[A-Z0-9_]+)",
    re.MULTILINE,
)
DASH_D = re.compile(r"-D(" + PREFIX + r"_[A-Z0-9_]+)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.source_root

    declared: set[str] = set()
    sources = [root / "CMakeLists.txt", *sorted((root / "cmake").glob("*.cmake"))]
    firmware = root / "firmware"
    if firmware.is_dir():
        sources.extend(sorted(firmware.rglob("CMakeLists.txt")))
        sources.extend(sorted(firmware.rglob("*.cmake")))
    for source in sources:
        for first, second in DECLARED.findall(source.read_text(encoding="utf-8")):
            declared.add(first or second)

    documentation = [root / "README.md", root / "CONTRIBUTING.md", root / "examples" / "README.md"]
    documentation.extend(sorted((root / "docs").glob("*.md")))
    failures: list[str] = []
    for path in documentation:
        if not path.exists():
            continue
        name = path.relative_to(root)
        text = path.read_text(encoding="utf-8")
        if TYPO in text:
            failures.append(f"{name}: contains the misspelled CMake option prefix {TYPO!r}")
        mentioned = set(DASH_D.findall(text)) | set(OPTION_SHAPE.findall(text))
        for option in sorted(mentioned - declared):
            failures.append(f"{name}: {option} is not declared by the CMake files")

    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
