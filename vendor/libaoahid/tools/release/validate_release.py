#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Reject release metadata that is inconsistent with the source tree."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

import package_source
import version_files

VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
REPOSITORY_URL = package_source.REPOSITORY_URL
ABI_NAMESPACE_BY_SOVERSION = {
    # ABI version nodes are persistent loader contracts, not package-version
    # labels: all compatible 2.x releases retain the AOAHID_2.0 node. A new
    # SOVERSION changes the SONAME and needs its own entry here. 1.x keeps its
    # entry so that a 1.x checkout still validates.
    "1": "1.0",
    "2": "2.0",
}


class ValidationError(RuntimeError):
    pass


def require_match(pattern: str, text: str, label: str, flags: int = 0) -> re.Match[str]:
    match = re.search(pattern, text, flags)
    if match is None:
        raise ValidationError(f"could not read {label}")
    return match


def validate_abi_namespace(
    project_version: str, soversion: str, abi_namespace: str
) -> None:
    project_major = project_version.split(".", maxsplit=1)[0]
    if soversion != project_major:
        raise ValidationError(
            f"CMake SOVERSION {soversion} disagrees with project major {project_major}"
        )
    expected_namespace = ABI_NAMESPACE_BY_SOVERSION.get(soversion)
    if expected_namespace is None:
        raise ValidationError(
            f"no ELF ABI namespace policy is defined for CMake SOVERSION {soversion}"
        )
    if abi_namespace != expected_namespace:
        raise ValidationError(
            f"ELF ABI namespace {abi_namespace} disagrees with stable namespace "
            f"{expected_namespace} for CMake SOVERSION {soversion}"
        )


def source_versions(root: Path) -> dict[str, str]:
    """Every file version_files.py tracks, plus the ABI namespace check.

    version_files.py is the single list of sites; sync_version.py writes it
    and this function reads it, so adding a new site (a new binding, a new
    generated file) only means editing that one module.
    """
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    version_script = (root / "cmake/aoahid.map").read_text(encoding="utf-8")

    try:
        assembled = version_files.assembled_versions(root)
    except version_files.VersionFileError as error:
        raise ValidationError(str(error)) from error

    cmake_version = assembled["CMakeLists.txt"]
    soversion = require_match(
        r"^\s*SOVERSION\s+([0-9]+)\s*$",
        cmake,
        "CMake SOVERSION",
        re.MULTILINE,
    ).group(1)
    abi_namespace = require_match(
        r"^\s*AOAHID_([0-9]+\.[0-9]+)\s*\{",
        version_script,
        "ELF ABI version namespace",
        re.MULTILINE,
    ).group(1)
    validate_abi_namespace(cmake_version, soversion, abi_namespace)

    return assembled


def validate_repository_identity(root: Path) -> None:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    cmake_config = (root / "cmake/aoahid-config.cmake.in").read_text(
        encoding="utf-8"
    )
    pkg_config = (root / "cmake/aoahid.pc.in").read_text(encoding="utf-8")
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    workflow = (root / ".github/workflows/release.yml").read_text(encoding="utf-8")
    readme = (root / "README.md").read_text(encoding="utf-8")
    security = (root / "SECURITY.md").read_text(encoding="utf-8")
    contributing = (root / "CONTRIBUTING.md").read_text(encoding="utf-8")
    expected_cmake = f'HOMEPAGE_URL "{REPOSITORY_URL}"'
    if expected_cmake not in cmake:
        raise ValidationError("CMake project homepage is not the canonical repository")
    if "URL: @PROJECT_HOMEPAGE_URL@" not in pkg_config:
        raise ValidationError("pkg-config metadata does not expose the CMake project homepage")
    if 'set(aoahid_HOMEPAGE_URL "@PROJECT_HOMEPAGE_URL@")' not in cmake_config:
        raise ValidationError("CMake package config does not expose the project homepage")
    if manifest.get("homepage") != REPOSITORY_URL:
        raise ValidationError("vcpkg homepage is not the canonical repository")
    if "AOAHID_EXPECTED_REPOSITORY: nemarpuc/libaoahid" not in workflow:
        raise ValidationError("release workflow is not bound to the intended repository")
    if f"[Canonical repository]({REPOSITORY_URL})" not in readme:
        raise ValidationError("README does not link the canonical repository")
    if f"{REPOSITORY_URL}/security/advisories/new" not in security:
        raise ValidationError("SECURITY does not link the canonical advisory form")
    if (
        f"{REPOSITORY_URL}/issues" not in contributing
        or f"{REPOSITORY_URL}/pulls" not in contributing
    ):
        raise ValidationError("CONTRIBUTING does not link canonical issues and pulls")


def release_notes(root: Path, version: str) -> str:
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    heading = re.compile(
        rf"^## \[{re.escape(version)}\] - ([0-9]{{4}}-[0-9]{{2}}-[0-9]{{2}})\s*$",
        re.MULTILINE,
    )
    match = heading.search(changelog)
    if match is None:
        raise ValidationError(f"CHANGELOG.md has no dated [{version}] section")
    next_heading = re.search(r"^## \[", changelog[match.end() :], re.MULTILINE)
    end = match.end() + next_heading.start() if next_heading else len(changelog)
    body = changelog[match.end() : end].strip()
    if not body:
        raise ValidationError(f"CHANGELOG.md [{version}] section is empty")
    notes = f"## libaoahid {version}\n\n{body}\n"
    if re.search(r"\bAndroid (?:is )?supported\b", notes, re.IGNORECASE):
        raise ValidationError("release notes contain an unqualified Android support claim")
    if "hardware-verified" not in notes.lower():
        raise ValidationError("release notes must state the hardware-verification status")
    return notes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--tag", required=True)
    parser.add_argument("--notes-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        tag_match = re.fullmatch(r"v(.+)", args.tag)
        if tag_match is None or VERSION_RE.fullmatch(tag_match.group(1)) is None:
            raise ValidationError("tag must have the exact form vMAJOR.MINOR.PATCH")
        version = tag_match.group(1)
        validate_repository_identity(args.root)
        versions = source_versions(args.root)
        differing = {source: value for source, value in versions.items() if value != version}
        if differing:
            rendered = ", ".join(f"{source}={value}" for source, value in differing.items())
            raise ValidationError(f"tag version {version} disagrees with {rendered}")
        notes = release_notes(args.root, version)
        if args.notes_out:
            args.notes_out.parent.mkdir(parents=True, exist_ok=True)
            args.notes_out.write_text(notes, encoding="utf-8")
    except (OSError, json.JSONDecodeError, ValidationError) as error:
        print(f"release-validation: error: {error}", file=sys.stderr)
        return 1
    print(f"release-validation: {args.tag} is internally consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
