#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Require an adjacent, per-function contract for every exported C declaration."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include/aoahid.h"
EXPORT_RE = re.compile(
    r"AOAHID_API\s+(?P<return_type>[^;]*?)\bAOAHID_CALL\s+"
    r"(?P<name>aoahid_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*;",
    re.MULTILINE | re.DOTALL,
)
REQUIRED_LABELS = ("Ownership:", "Blocking:", "Synchronization:", "Returns:")


def adjacent_comment(text: str, declaration_start: int) -> str | None:
    close = text.rfind("*/", 0, declaration_start)
    if close < 0 or text[close + 2 : declaration_start].strip():
        return None
    opening = text.rfind("/*", 0, close)
    if opening < 0:
        return None
    return text[opening : close + 2]


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    matches = list(EXPORT_RE.finditer(text))
    failures: list[str] = []
    names: set[str] = set()

    for match in matches:
        name = match.group("name")
        if name in names:
            failures.append(f"{name}: duplicate exported declaration")
        names.add(name)

        comment = adjacent_comment(text, match.start())
        if comment is None:
            failures.append(f"{name}: no adjacent block comment")
            continue
        if not comment.startswith(f"/* {name}\n"):
            failures.append(f"{name}: comment must start with the exact function name")
        for label in REQUIRED_LABELS:
            if comment.count(label) != 1:
                failures.append(f"{name}: requires exactly one {label} field")
        if "aoahid_result" in match.group("return_type"):
            returns = comment.split("Returns:", 1)[-1]
            if "AOAHID_OK" not in returns or "AOAHID_ERR_" not in returns:
                failures.append(f"{name}: result contract must enumerate success and errors")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    if not matches:
        print(f"{HEADER}: no exported functions found", file=sys.stderr)
        return 1
    print(f"public contracts: {len(matches)} functions documented individually")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
