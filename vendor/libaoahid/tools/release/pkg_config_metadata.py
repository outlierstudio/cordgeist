#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Relocate and validate the pkg-config metadata shipped in release archives."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shlex
import sys


RELOCATABLE_PREFIX = "${pcfiledir}/../.."
UNCHECKED_PRIVATE_LIBRARIES = object()
UNCHECKED_URL = object()


class PkgConfigError(RuntimeError):
    pass


def _decode(contents: bytes, label: str) -> str:
    try:
        text = contents.decode("utf-8")
    except UnicodeDecodeError as error:
        raise PkgConfigError(f"{label}: metadata is not UTF-8: {error}") from error
    if "\x00" in text:
        raise PkgConfigError(f"{label}: metadata contains a NUL byte")
    return text


def _one_value(text: str, pattern: str, label: str) -> str:
    values = re.findall(pattern, text, re.MULTILINE)
    if len(values) != 1:
        raise PkgConfigError(f"expected exactly one {label}; found {len(values)}")
    return values[0].strip()


def validate_relocatable(
    contents: bytes,
    *,
    label: str,
    expected_name: str,
    expected_version: str,
    expected_library: str,
    expected_url: str | None | object = UNCHECKED_URL,
    expected_requires_private: str | None = None,
    expected_private_libraries: str | tuple[str, ...] | None | object = UNCHECKED_PRIVATE_LIBRARIES,
) -> None:
    text = _decode(contents, label)
    variables: dict[str, str | tuple[str, ...]] = {
        "prefix": RELOCATABLE_PREFIX,
        "exec_prefix": "${prefix}",
        "libdir": ("${exec_prefix}/lib", "${prefix}/lib"),
        "includedir": "${prefix}/include",
    }
    for variable, expected in variables.items():
        actual = _one_value(
            text,
            rf"^{re.escape(variable)}[ \t]*=[ \t]*(.*)$",
            f"{label} {variable} assignment",
        )
        accepted = (expected,) if isinstance(expected, str) else expected
        if actual not in accepted:
            raise PkgConfigError(
                f"{label}: {variable} is {actual!r}, expected relocatable {accepted!r}"
            )
    name = _one_value(text, r"^Name[ \t]*:[ \t]*(.*)$", f"{label} Name field")
    version = _one_value(text, r"^Version[ \t]*:[ \t]*(.*)$", f"{label} Version field")
    libraries = _one_value(text, r"^Libs[ \t]*:[ \t]*(.*)$", f"{label} Libs field")
    if name != expected_name:
        raise PkgConfigError(f"{label}: Name is {name!r}, expected {expected_name!r}")
    if version != expected_version:
        raise PkgConfigError(
            f"{label}: Version is {version!r}, expected {expected_version!r}"
        )
    urls = re.findall(r"^URL[ \t]*:[ \t]*(.*)$", text, re.MULTILINE)
    if expected_url is not UNCHECKED_URL:
        if expected_url is None:
            if urls:
                raise PkgConfigError(f"{label}: unexpected URL field")
        elif len(urls) != 1 or urls[0].strip() != expected_url:
            raise PkgConfigError(
                f"{label}: URL is {[value.strip() for value in urls]!r}, "
                f"expected exactly {expected_url!r}"
            )
    try:
        link_tokens = shlex.split(libraries, posix=True)
    except ValueError as error:
        raise PkgConfigError(f"{label}: Libs cannot be parsed: {error}") from error
    expected_link = ["-L${libdir}", f"-l{expected_library}"]
    if link_tokens != expected_link:
        raise PkgConfigError(
            f"{label}: Libs is {link_tokens!r}, expected {expected_link!r}"
        )
    requirements = re.findall(
        r"^Requires\.private[ \t]*:[ \t]*(.*)$", text, re.MULTILINE
    )
    if expected_requires_private is None:
        if requirements:
            raise PkgConfigError(f"{label}: shared metadata has Requires.private")
    else:
        if len(requirements) != 1:
            raise PkgConfigError(
                f"{label}: expected exactly one Requires.private field; "
                f"found {len(requirements)}"
            )
        requirement = requirements[0].strip()
        if requirement != expected_requires_private:
            raise PkgConfigError(
                f"{label}: Requires.private is {requirement!r}, "
                f"expected {expected_requires_private!r}"
            )
    private_libraries = [
        value.strip()
        for value in re.findall(r"^Libs\.private[ \t]*:[ \t]*(.*)$", text, re.MULTILINE)
        if value.strip()
    ]
    if expected_private_libraries is not UNCHECKED_PRIVATE_LIBRARIES:
        if expected_private_libraries is None:
            if private_libraries:
                raise PkgConfigError(f"{label}: shared metadata has Libs.private")
        else:
            accepted_private = (
                (expected_private_libraries,)
                if isinstance(expected_private_libraries, str)
                else expected_private_libraries
            )
            if len(private_libraries) != 1 or private_libraries[0] not in accepted_private:
                raise PkgConfigError(
                    f"{label}: Libs.private is {private_libraries!r}, "
                    f"expected one of {accepted_private!r}"
                )


def relocate(path: Path) -> None:
    text = _decode(path.read_bytes(), str(path))
    matches = list(re.finditer(r"^prefix[ \t]*=[ \t]*.*$", text, re.MULTILINE))
    if len(matches) != 1:
        raise PkgConfigError(
            f"{path}: expected exactly one prefix assignment; found {len(matches)}"
        )
    match = matches[0]
    rewritten = text[: match.start()] + f"prefix={RELOCATABLE_PREFIX}" + text[match.end() :]
    path.write_text(rewritten, encoding="utf-8", newline="")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--relocate", type=Path, required=True, metavar="PC_FILE")
    args = parser.parse_args()
    try:
        relocate(args.relocate)
    except (OSError, PkgConfigError) as error:
        print(f"pkg-config-metadata: error: {error}", file=sys.stderr)
        return 1
    print(f"pkg-config-metadata: relocated {args.relocate}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
