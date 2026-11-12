#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Inspect numeric GLIBC symbol requirements with GNU readelf version data."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


BASELINE = "ubuntu-22.04-glibc-2.35"
MAXIMUM_ALLOWED = "GLIBC_2.35"
GLIBC_TOKEN_RE = re.compile(r"GLIBC_[A-Za-z0-9_.-]+")
VERSION_RE = re.compile(r"GLIBC_[0-9]+(?:\.[0-9]+)+")


class GlibcVersionError(RuntimeError):
    pass


def version_key(value: str) -> tuple[int, ...]:
    match = re.fullmatch(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", value)
    if match is None:
        raise GlibcVersionError(f"invalid numeric GLIBC version {value!r}")
    return tuple(int(component) for component in match.group(1).split("."))


def inspect(readelf: str, path: Path) -> list[str]:
    completed = subprocess.run(
        [readelf, "--version-info", "--wide", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise GlibcVersionError(f"readelf failed for {path}: {detail}")
    tokens = set(GLIBC_TOKEN_RE.findall(completed.stdout))
    unsupported = sorted(token for token in tokens if VERSION_RE.fullmatch(token) is None)
    if unsupported:
        raise GlibcVersionError(
            f"readelf reported unsupported nonnumeric GLIBC requirements for {path}: "
            + ", ".join(unsupported)
        )
    versions = sorted(tokens, key=version_key)
    if not versions:
        raise GlibcVersionError(f"readelf reported no numeric GLIBC requirements for {path}")
    return versions


def create_document(root: Path, readelf: str, paths: list[Path], maximum: str) -> dict[str, object]:
    root = root.resolve()
    maximum_key = version_key(maximum)
    resolved = [path.resolve() for path in paths]
    if not resolved:
        raise GlibcVersionError("at least one shared-library path is required")
    if len(resolved) != len(set(resolved)):
        raise GlibcVersionError("duplicate shared-library paths were supplied")
    files: list[dict[str, object]] = []
    union: set[str] = set()
    for path in sorted(resolved):
        if not path.is_file():
            raise GlibcVersionError(f"shared-library path is not a regular file: {path}")
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError as error:
            raise GlibcVersionError(f"{path} is outside inspection root {root}") from error
        versions = inspect(readelf, path)
        if version_key(versions[-1]) > maximum_key:
            raise GlibcVersionError(
                f"{relative} requires {versions[-1]}, above baseline maximum {maximum}"
            )
        union.update(versions)
        files.append(
            {
                "path": relative,
                "required_versions": versions,
                "maximum_required": versions[-1],
            }
        )
    all_versions = sorted(union, key=version_key)
    return {
        "schema": 1,
        "baseline": BASELINE,
        "maximum_allowed": maximum,
        "required_versions": all_versions,
        "maximum_required": all_versions[-1],
        "files": files,
    }


def validate_document(
    document: object,
    *,
    expected_paths: set[str] | None = None,
    maximum: str = MAXIMUM_ALLOWED,
) -> str:
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise GlibcVersionError("glibc requirements document has an invalid schema")
    if document.get("baseline") != BASELINE or document.get("maximum_allowed") != maximum:
        raise GlibcVersionError("glibc requirements document has the wrong baseline")
    files = document.get("files")
    if not isinstance(files, list) or not files:
        raise GlibcVersionError("glibc requirements document has no inspected files")
    actual_paths: set[str] = set()
    union: set[str] = set()
    for item in files:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise GlibcVersionError("glibc requirements file record is invalid")
        path = str(item["path"])
        if path.startswith("/") or ".." in Path(path).parts or path in actual_paths:
            raise GlibcVersionError(f"invalid or duplicate inspected path {path!r}")
        actual_paths.add(path)
        versions = item.get("required_versions")
        if not isinstance(versions, list) or not versions or not all(
            isinstance(value, str) for value in versions
        ):
            raise GlibcVersionError(f"{path} has no GLIBC version requirements")
        normalized = sorted(set(versions), key=version_key)
        if versions != normalized or item.get("maximum_required") != normalized[-1]:
            raise GlibcVersionError(f"{path} GLIBC requirements are not canonical")
        if version_key(normalized[-1]) > version_key(maximum):
            raise GlibcVersionError(f"{path} exceeds GLIBC baseline {maximum}")
        union.update(normalized)
    if expected_paths is not None and actual_paths != expected_paths:
        raise GlibcVersionError(
            f"inspected shared libraries differ: actual={sorted(actual_paths)!r}, "
            f"expected={sorted(expected_paths)!r}"
        )
    all_versions = sorted(union, key=version_key)
    if document.get("required_versions") != all_versions:
        raise GlibcVersionError("aggregate GLIBC requirements are inconsistent")
    maximum_required = all_versions[-1]
    if document.get("maximum_required") != maximum_required:
        raise GlibcVersionError("aggregate maximum GLIBC requirement is inconsistent")
    return maximum_required


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--maximum", default=MAXIMUM_ALLOWED)
    parser.add_argument("libraries", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        document = create_document(args.root, args.readelf, args.libraries, args.maximum)
        validate_document(document, maximum=args.maximum)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (GlibcVersionError, OSError) as error:
        print(f"glibc-version-audit: error: {error}", file=sys.stderr)
        return 1
    print(
        "glibc-version-audit: "
        f"{len(document['files'])} files; maximum {document['maximum_required']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
