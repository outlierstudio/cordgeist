#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Write one release version into every file that must carry it.

Usage: python tools/release/sync_version.py 0.6.0

This replaces manually editing CMakeLists.txt, include/aoahid.h, vcpkg.json,
tools/docs/Doxyfile, the Python/C#/Rust binding manifests and
tests/abi/check_abi_golden.py's golden version constants one at a time. The
version constants inside the generated bindings follow the header: this script
runs tools/bindings/generate.py (Clang) after the sites are written. version_files.py holds the single list of sites;
this script only drives it and reports what changed.

It does not touch CHANGELOG.md: the release notes for the new version are
prose the developer must still write, and validate_release.py enforces that
a dated section exists before a tag is accepted.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

import version_files


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="new release version, e.g. 0.6.0")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--no-generate", action="store_true",
                        help="do not regenerate the bindings (they then still carry the old constants)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        changed = version_files.write_all(args.root, args.version)
    except (OSError, version_files.VersionFileError) as error:
        print(f"sync-version: error: {error}", file=sys.stderr)
        return 1
    if not changed:
        print(f"sync-version: every site already reads {args.version}")
        return 0
    if not args.no_generate:
        # The constants inside the generated bindings follow the header; this needs Clang.
        generator = args.root / "tools/bindings/generate.py"
        if subprocess.call([sys.executable, str(generator)]) != 0:
            print("sync-version: error: tools/bindings/generate.py failed; fix it and run it",
                  file=sys.stderr)
            return 1
    for path in sorted(changed):
        print(f"sync-version: updated {path}")
    print(f"sync-version: {len(changed)} file(s) now read {args.version}")
    print(
        "sync-version: CHANGELOG.md is unchanged; add a dated "
        f"## [{args.version}] section with its own hardware-verification "
        "status before tagging."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
