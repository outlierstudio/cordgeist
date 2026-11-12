#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Require an SPDX-License-Identifier and copyright line in every source file.

The repository root LICENSE covers the whole tree, but a single file copied out
of it carries no license information of its own. Machine license scanners and
SBOM generators also work per file. This check therefore requires the two-line
SPDX header on every first-party source file in the extensions listed below.

Data and markup formats are deliberately out of scope: JSON cannot carry a
comment.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
SPDX_LINE = "SPDX-License-Identifier: MIT"
COPYRIGHT_LINE = "Copyright (c) 2026 libaoahid contributors"

# Comment syntax is a property of the language, not of the file's role. Kotlin
# build scripts are Kotlin source and reject a '#' line outright, so the two
# tables below must stay keyed on what each parser accepts.
SLASH_COMMENT_SUFFIXES = frozenset(
    {".c", ".h", ".cpp", ".hpp", ".rs", ".cs", ".java", ".kt", ".kts"}
)
HASH_COMMENT_SUFFIXES = frozenset({".py", ".cmake", ".sh"})
HASH_COMMENT_NAMES = frozenset({"CMakeLists.txt"})

# Directories with no first-party source.
EXCLUDED_DIRECTORIES = ("build", ".git")


def in_scope(path: Path) -> bool:
    relative = path.relative_to(ROOT).as_posix()
    if any(
        relative == excluded or relative.startswith(excluded + "/")
        for excluded in EXCLUDED_DIRECTORIES
    ):
        return False
    return (
        path.suffix in SLASH_COMMENT_SUFFIXES
        or path.suffix in HASH_COMMENT_SUFFIXES
        or path.name in HASH_COMMENT_NAMES
    )


def sources() -> list[Path]:
    return sorted(path for path in ROOT.rglob("*") if path.is_file() and in_scope(path))


def header_prefix(path: Path) -> str:
    if path.suffix in SLASH_COMMENT_SUFFIXES:
        return "// "
    return "# "


def check(path: Path) -> str | None:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        return f"{path.relative_to(ROOT)}: unreadable ({error})"
    prefix = header_prefix(path)
    # A shebang must stay on the first physical line, so accept the header
    # immediately after it.
    lines = text.splitlines()
    start = 1 if lines and lines[0].startswith("#!") else 0
    window = lines[start : start + 2]
    expected = [prefix + SPDX_LINE, prefix + COPYRIGHT_LINE]
    if window != expected:
        return (
            f"{path.relative_to(ROOT)}: expected the exact two-line header "
            f"{expected[0]!r} then {expected[1]!r}"
        )
    return None


def insert(path: Path) -> bool:
    if check(path) is None:
        return False
    text = path.read_text(encoding="utf-8")
    prefix = header_prefix(path)
    header = f"{prefix}{SPDX_LINE}\n{prefix}{COPYRIGHT_LINE}\n"
    if text.startswith("#!"):
        shebang, separator, rest = text.partition("\n")
        path.write_text(shebang + separator + header + rest, encoding="utf-8")
    else:
        path.write_text(header + text, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fix",
        action="store_true",
        help="Insert the header into every file that is missing it.",
    )
    arguments = parser.parse_args()
    files = sources()
    if not files:
        print("check-license-headers: no source files were found", file=sys.stderr)
        return 1
    if arguments.fix:
        changed = [path for path in files if insert(path)]
        for path in changed:
            print(f"check-license-headers: added header to {path.relative_to(ROOT)}")
    failures = [message for message in (check(path) for path in files) if message]
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"check-license-headers: {len(files)} source files carry the MIT SPDX header")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
