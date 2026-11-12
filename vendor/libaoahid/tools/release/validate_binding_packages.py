#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Validate the registry packages without loading an untrusted native library."""

from __future__ import annotations

import argparse
from email.parser import BytesParser
from pathlib import Path, PurePosixPath
import re
import sys
import zipfile
import xml.etree.ElementTree as ET


class BindingPackageError(RuntimeError):
    pass


def one_file(directory: Path, pattern: str, label: str) -> Path:
    matches = sorted(path for path in directory.glob(pattern) if path.is_file())
    if len(matches) != 1:
        rendered = ", ".join(path.name for path in matches) or "none"
        raise BindingPackageError(f"expected exactly one {label}; found {rendered}")
    return matches[0]


def safe_members(archive: zipfile.ZipFile, label: str) -> list[str]:
    members = [item.filename for item in archive.infolist() if not item.is_dir()]
    if len(members) != len(set(members)):
        raise BindingPackageError(f"{label} contains duplicate archive members")
    for member in members:
        path = PurePosixPath(member)
        if path.is_absolute() or not path.parts or ".." in path.parts:
            raise BindingPackageError(f"{label} contains unsafe member {member!r}")
    return members


def validate_wheel(directory: Path, version: str) -> Path:
    wheel = one_file(directory, "*.whl", "Python wheel")
    expected_name = f"aoahid-{version}-py3-none-any.whl"
    if wheel.name != expected_name:
        raise BindingPackageError(
            f"Python wheel is {wheel.name!r}, expected {expected_name!r}"
        )
    with zipfile.ZipFile(wheel) as archive:
        members = safe_members(archive, wheel.name)
        for suffix in (".so", ".dll", ".dylib", ".pyd", ".a", ".lib"):
            if any(member.lower().endswith(suffix) for member in members):
                raise BindingPackageError(
                    f"platform-independent wheel contains a native {suffix} artifact"
                )
        for required in ("aoahid/__init__.py", "aoahid/native.py"):
            if required not in members:
                raise BindingPackageError(f"Python wheel is missing {required}")
        metadata_names = [name for name in members if name.endswith(".dist-info/METADATA")]
        wheel_names = [name for name in members if name.endswith(".dist-info/WHEEL")]
        record_names = [name for name in members if name.endswith(".dist-info/RECORD")]
        license_names = [
            name for name in members if re.search(r"\.dist-info/licenses/LICENSE$", name)
        ]
        if not (
            len(metadata_names) == len(wheel_names) == len(record_names) == len(license_names) == 1
        ):
            raise BindingPackageError("Python wheel metadata/license layout is incomplete")
        metadata_bytes = archive.read(metadata_names[0])
        metadata = BytesParser().parsebytes(metadata_bytes)
        if metadata.get("Name") != "aoahid" or metadata.get("Version") != version:
            raise BindingPackageError("Python wheel Name/Version metadata is incorrect")
        if metadata.get("License-Expression") != "MIT":
            raise BindingPackageError("Python wheel has no exact MIT License-Expression")
        description = metadata.get_payload(decode=True)
        if description is None or b"contains no `.so`, `.dll`, `.dylib`, or" not in description:
            raise BindingPackageError("Python wheel does not disclose its external native dependency")
        wheel_metadata = archive.read(wheel_names[0]).decode("utf-8")
        if not re.search(r"^Root-Is-Purelib: true$", wheel_metadata, re.MULTILINE):
            raise BindingPackageError("Python wheel is not marked as purelib")
        if not re.search(r"^Tag: py3-none-any$", wheel_metadata, re.MULTILINE):
            raise BindingPackageError("Python wheel does not have the exact py3-none-any tag")
    return wheel


def element_text(root: ET.Element, name: str) -> str | None:
    element = root.find(f".//{{*}}{name}")
    return element.text.strip() if element is not None and element.text else None


def validate_nuget(directory: Path, version: str) -> Path:
    package = one_file(directory, "*.nupkg", "NuGet package")
    expected_name = f"AoaHid.{version}.nupkg"
    if package.name != expected_name:
        raise BindingPackageError(
            f"NuGet package is {package.name!r}, expected {expected_name!r}"
        )
    with zipfile.ZipFile(package) as archive:
        members = safe_members(archive, package.name)
        nuspecs = [name for name in members if name.endswith(".nuspec")]
        if len(nuspecs) != 1:
            raise BindingPackageError("NuGet package must contain exactly one nuspec")
        try:
            nuspec = ET.fromstring(archive.read(nuspecs[0]))
        except ET.ParseError as error:
            raise BindingPackageError(f"NuGet nuspec is invalid XML: {error}") from error
        expected_fields = {
            "id": "AoaHid",
            "version": version,
            "authors": "libaoahid contributors",
            "license": "MIT",
            "readme": "README.md",
        }
        for field, expected in expected_fields.items():
            if element_text(nuspec, field) != expected:
                raise BindingPackageError(
                    f"NuGet {field} is {element_text(nuspec, field)!r}, expected {expected!r}"
                )
        description = element_text(nuspec, "description") or ""
        if "separately installed, version-matched native libaoahid" not in description:
            raise BindingPackageError("NuGet metadata does not disclose its native dependency")
        required = {"README.md", "LICENSE", "lib/net8.0/AoaHid.dll"}
        missing = required - set(members)
        if missing:
            raise BindingPackageError(
                f"NuGet package is missing {', '.join(sorted(missing))}"
            )
        if any(member.startswith("runtimes/") for member in members):
            raise BindingPackageError("NuGet declaration package unexpectedly contains runtime assets")
        native_suffixes = (".so", ".dylib", ".a", ".lib")
        if any(member.lower().endswith(native_suffixes) for member in members):
            raise BindingPackageError("NuGet declaration package contains a native library")
        dlls = [name for name in members if name.lower().endswith(".dll")]
        if dlls != ["lib/net8.0/AoaHid.dll"]:
            raise BindingPackageError(f"NuGet package has unexpected DLL members: {dlls!r}")
        readme = archive.read("README.md").decode("utf-8")
        if "does not contain `aoahid.dll`, `libaoahid.so`, libusb" not in readme:
            raise BindingPackageError("NuGet readme does not state that native assets are external")
    return package


def validate_csharp_project(path: Path) -> None:
    try:
        project = ET.fromstring(path.read_bytes())
    except ET.ParseError as error:
        raise BindingPackageError(f"C# project is invalid XML: {error}") from error
    package_references = [
        element for element in project.iter() if element.tag.rsplit("}", 1)[-1] == "PackageReference"
    ]
    if package_references:
        raise BindingPackageError(
            "C# binding project has external PackageReference dependencies"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python-directory", type=Path, required=True)
    parser.add_argument("--nuget-directory", type=Path, required=True)
    parser.add_argument("--csharp-project", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version) is None:
            raise BindingPackageError("version must have the exact form MAJOR.MINOR.PATCH")
        wheel = validate_wheel(args.python_directory, args.version)
        validate_csharp_project(args.csharp_project)
        package = validate_nuget(args.nuget_directory, args.version)
    except (BindingPackageError, OSError, zipfile.BadZipFile) as error:
        print(f"binding-package-validation: error: {error}", file=sys.stderr)
        return 1
    print(f"binding-package-validation: {wheel.name} and {package.name} are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
