#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""The single list of files that must agree on the release version.

validate_release.py reads this list to reject a mismatch; sync_version.py
writes this list to apply a new version everywhere at once. Adding a new
place that carries the version string means adding one VersionSite here,
not touching both scripts.
"""

from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import Path

VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


class VersionFileError(RuntimeError):
    pass


@dataclass(frozen=True)
class VersionSite:
    """One occurrence of the release version inside one file.

    `pattern` must have exactly one capture group spanning only the version
    text (e.g. "1.2.3", or a single component when `component` narrows it).
    `component` is "full" for a site holding the whole X.Y.Z string, or
    "major"/"minor"/"patch" for a site holding just that numeral (used by
    files that spell the header's three separate #define lines).
    """

    path: str
    pattern: str
    component: str = "full"
    flags: int = re.MULTILINE

    def _regex(self) -> re.Pattern[str]:
        return re.compile(self.pattern, self.flags)

    def read(self, text: str) -> str:
        match = self._regex().search(text)
        if match is None:
            raise VersionFileError(f"{self.path}: could not find version pattern")
        return match.group(1)

    def write(self, text: str, new_version: str) -> str:
        major, minor, patch = new_version.split(".")
        replacement = {"full": new_version, "major": major, "minor": minor, "patch": patch}[
            self.component
        ]
        regex = self._regex()
        match = regex.search(text)
        if match is None:
            raise VersionFileError(f"{self.path}: could not find version pattern")
        start, end = match.span(1)
        return text[:start] + replacement + text[end:]


# Every location a release version must match. The constants inside the generated binding files
# are not listed: they follow include/aoahid.h through tools/bindings/generate.py, whose --check
# pins them. Order is documentation order,
# not evaluation order.
SITES: tuple[VersionSite, ...] = (
    VersionSite("CMakeLists.txt", r"project\(libaoahid VERSION (\d+\.\d+\.\d+)"),
    VersionSite("include/aoahid.h", r"^#define AOAHID_VERSION_MAJOR (\d+)$", "major"),
    VersionSite("include/aoahid.h", r"^#define AOAHID_VERSION_MINOR (\d+)$", "minor"),
    VersionSite("include/aoahid.h", r"^#define AOAHID_VERSION_PATCH (\d+)$", "patch"),
    VersionSite("vcpkg.json", r'"version-semver":\s*"(\d+\.\d+\.\d+)"'),
    VersionSite("tools/docs/Doxyfile", r"^PROJECT_NUMBER\s*=\s*(\d+\.\d+\.\d+)\s*$"),
    VersionSite("bindings/python/pyproject.toml", r'^version = "(\d+\.\d+\.\d+)"$'),
    VersionSite("bindings/csharp/AoaHid/AoaHid.csproj", r"<Version>(\d+\.\d+\.\d+)</Version>"),
    VersionSite("bindings/rust/aoahid/Cargo.toml", r'^version = "(\d+\.\d+\.\d+)"$'),
    VersionSite(
        "bindings/rust/aoahid/Cargo.toml",
        r'aoahid-sys = \{ path = "\.\./aoahid-sys", version = "(\d+\.\d+\.\d+)" \}',
    ),
    VersionSite("bindings/rust/aoahid-sys/Cargo.toml", r'^version = "(\d+\.\d+\.\d+)"$'),
    VersionSite(
        "tests/abi/check_abi_golden.py",
        r'"AOAHID_VERSION_MAJOR": (\d+),',
        "major",
    ),
    VersionSite(
        "tests/abi/check_abi_golden.py",
        r'"AOAHID_VERSION_MINOR": (\d+),',
        "minor",
    ),
    VersionSite(
        "tests/abi/check_abi_golden.py",
        r'"AOAHID_VERSION_PATCH": (\d+),',
        "patch",
    ),
)


def read_all(root: Path) -> list[tuple[VersionSite, str]]:
    """Read every site's raw text: a full version string, or one component."""
    cache: dict[str, str] = {}
    results: list[tuple[VersionSite, str]] = []
    for site in SITES:
        text = cache.setdefault(site.path, (root / site.path).read_text(encoding="utf-8"))
        results.append((site, site.read(text)))
    return results


def assembled_versions(root: Path) -> dict[str, str]:
    """Collapse every site into one X.Y.Z string per file.

    A file may carry the full version in more than one place (e.g. a Rust
    manifest's own version and its pinned path-dependency version); those
    must already agree, and disagreement is reported rather than silently
    picking one.
    """
    by_file: dict[str, dict[str, str]] = {}
    for site, value in read_all(root):
        parts = by_file.setdefault(site.path, {})
        if site.component == "full":
            existing = parts.get("full")
            if existing is not None and existing != value:
                raise VersionFileError(
                    f"{site.path}: multiple version occurrences disagree: "
                    f"{existing!r} vs {value!r}"
                )
            parts["full"] = value
        else:
            parts[site.component] = value
    assembled: dict[str, str] = {}
    for path, components in by_file.items():
        if "full" in components:
            assembled[path] = components["full"]
        else:
            assembled[path] = ".".join(
                components[part] for part in ("major", "minor", "patch")
            )
    return assembled


def write_all(root: Path, new_version: str) -> list[str]:
    """Write new_version into every site. Returns the changed relative paths."""
    if VERSION_RE.fullmatch(new_version) is None:
        raise VersionFileError(f"not a MAJOR.MINOR.PATCH version: {new_version!r}")
    by_path: dict[str, list[VersionSite]] = {}
    for site in SITES:
        by_path.setdefault(site.path, []).append(site)
    changed: list[str] = []
    for path, sites in by_path.items():
        full_path = root / path
        original = full_path.read_text(encoding="utf-8")
        text = original
        for site in sites:
            text = site.write(text, new_version)
        if text != original:
            full_path.write_text(text, encoding="utf-8")
            changed.append(path)
    return changed
