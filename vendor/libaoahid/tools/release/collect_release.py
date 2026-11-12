#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Validate the complete six-target release set and write checksums."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
import tarfile
import zipfile

import generate_spdx
import glibc_versions
import package_source
import pkg_config_metadata


TARGETS = (
    ("linux", "x86_64", "ubuntu22.04", ".tar.gz"),
    ("linux", "aarch64", "ubuntu22.04", ".tar.gz"),
    ("windows", "x86_64", "", ".zip"),
    ("windows", "arm64", "", ".zip"),
    ("macos", "arm64", "", ".tar.gz"),
    ("macos", "x86_64", "", ".tar.gz"),
)
# Platforms whose archives are .tar.gz; Windows alone ships .zip.
TAR_PLATFORMS = ("linux", "macos")
VARIANTS = ("shared", "static")
LIBUSB_SOURCE_NAME = "share/doc/libaoahid/third-party/source/libusb-1.0.30.tar.bz2"
LIBUSB_SOURCE_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_LICENSE_NAME = "share/doc/libaoahid/third-party/libusb-copyright"
LIBUSB_LICENSE_SHA256 = "5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a"
VCPKG_LICENSE_NAME = "share/doc/libaoahid/third-party/vcpkg-LICENSE.txt"
VCPKG_LICENSE_SHA256 = "1ee376fc340e0aa6ad6a3581c94126e741468705096ac92263048a21daa86460"

# A runtime bundle is the smallest asset that may be redistributed on its own.
# libaoahid links libusb dynamically, so any asset carrying the libusb runtime
# must carry libusb's license and its corresponding source in the same
# distribution unit. A bare shared object cannot, which is why one is never
# uploaded.
RUNTIME_BUNDLE_DOCUMENTS = (
    "share/doc/libaoahid/LICENSE",
    "share/doc/libaoahid/NOTICE",
    "share/doc/libaoahid/THIRD_PARTY_NOTICES.md",
    "share/doc/libaoahid/build-metadata.json",
    LIBUSB_LICENSE_NAME,
    LIBUSB_SOURCE_NAME,
)
WINDOWS_RUNTIME_BUNDLE_DOCUMENTS = (
    VCPKG_LICENSE_NAME,
    "share/doc/libaoahid/third-party/vcpkg-libusb-port/portfile.cmake",
    "share/doc/libaoahid/third-party/vcpkg-libusb-port/vcpkg.json",
)
RUNTIME_BUNDLE_README_NAME = "README.txt"
RUNTIME_BUNDLE_README = """\
libaoahid runtime bundle
========================

This archive carries only the shared libraries needed to run an application
that is already built against libaoahid, together with the license material
that must travel with them. It has no headers, no import library, and no
CMake or pkg-config metadata; build against the matching -shared or -static
archive from the same release instead.

Contents
--------

  lib/ or bin/   the libaoahid shared library and the libusb-1.0 runtime it
                 loads at run time
  share/doc/libaoahid/LICENSE
                 the MIT license covering libaoahid itself
  share/doc/libaoahid/NOTICE
  share/doc/libaoahid/THIRD_PARTY_NOTICES.md
                 the exact third-party versions, licenses, and checksums
  share/doc/libaoahid/third-party/libusb-copyright
                 the full LGPL-2.1 text covering the bundled libusb runtime
  share/doc/libaoahid/third-party/source/libusb-1.0.30.tar.bz2
                 the official corresponding source for that libusb runtime
  share/doc/libaoahid/build-metadata.json
                 the version, commit, platform, and libusb version recorded at
                 build time

libaoahid is MIT licensed. libusb is a separate work under LGPL-2.1-or-later
and is linked dynamically; it is never absorbed into libaoahid. Redistribute
this archive unchanged and both licenses remain satisfied as shipped. If you
replace the bundled libusb with a modified copy, the LGPL obligations apply to
that copy.
"""


class CollectionError(RuntimeError):
    pass


def platform_part(platform: str, arch: str, qualifier: str) -> str:
    return f"{platform}-{arch}" + (f"-{qualifier}" if qualifier else "")


def runtime_bundle_name(
    version: str, platform: str, arch: str, qualifier: str
) -> str:
    """Return the license-complete runtime asset name for one target."""
    extension = ".tar.gz" if platform in TAR_PLATFORMS else ".zip"
    return (
        f"libaoahid-{version}-{platform_part(platform, arch, qualifier)}"
        f"-runtime{extension}"
    )


def expected_release_asset_names(version: str) -> frozenset[str]:
    """Return the exact set of files uploaded to one GitHub Release."""
    names = {
        package_source.source_archive_name(version),
        "release-manifest.json",
        "SHA256SUMS",
    }
    for platform, arch, qualifier, extension in TARGETS:
        part = platform_part(platform, arch, qualifier)
        for variant in VARIANTS:
            base = f"libaoahid-{version}-{part}-{variant}"
            names.update((base + extension, base + ".spdx.json"))
        names.add(runtime_bundle_name(version, platform, arch, qualifier))
    return frozenset(names)


def sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def archive_members(path: Path) -> set[str]:
    expected_root = path.name.removesuffix(".tar.gz").removesuffix(".zip")
    if path.name.endswith(".tar.gz"):
        with tarfile.open(path, "r:gz") as archive:
            selected = [member for member in archive.getmembers() if member.isfile() or member.issym()]
            for member in selected:
                if member.issym():
                    target = PurePosixPath(member.linkname)
                    if target.is_absolute() or ".." in target.parts:
                        raise CollectionError(f"unsafe symlink in {path.name}: {member.name} -> {member.linkname}")
            raw = [member.name for member in selected]
    else:
        with zipfile.ZipFile(path) as archive:
            raw = [member.filename for member in archive.infolist() if not member.is_dir()]
    normalized: set[str] = set()
    for member in raw:
        parts = PurePosixPath(member).parts
        if len(parts) < 2 or parts[0] in ("", ".", "..") or ".." in parts:
            raise CollectionError(f"unsafe or rootless archive member in {path.name}: {member}")
        if parts[0] != expected_root:
            raise CollectionError(f"{path.name}: unexpected top-level directory {parts[0]}")
        normalized.add(PurePosixPath(*parts[1:]).as_posix())
    if len(normalized) != len(raw):
        raise CollectionError(f"{path.name}: duplicate archive member")
    return normalized


def archive_symlinks(path: Path) -> dict[str, str]:
    """Return the symbolic-link members of one archive and their targets."""
    if not path.name.endswith(".tar.gz"):
        return {}
    root = path.name.removesuffix(".tar.gz")
    with tarfile.open(path, "r:gz") as archive:
        return {
            PurePosixPath(member.name).relative_to(root).as_posix(): member.linkname
            for member in archive.getmembers()
            if member.issym()
        }


def archive_bytes(path: Path, relative: str) -> bytes:
    root = path.name.removesuffix(".tar.gz").removesuffix(".zip")
    member_name = f"{root}/{relative}"
    if path.name.endswith(".tar.gz"):
        with tarfile.open(path, "r:gz") as archive:
            member = archive.getmember(member_name)
            stream = archive.extractfile(member)
            if stream is None:
                raise CollectionError(f"{path.name}: {relative} is not a regular file")
            return stream.read()
    with zipfile.ZipFile(path) as archive:
        return archive.read(member_name)


def shared_runtime(path: Path, platform: str) -> tuple[str, bytes]:
    root = path.name.removesuffix(".tar.gz").removesuffix(".zip")
    if platform in TAR_PLATFORMS:
        runtime_pattern = (
            r"libaoahid\.so\.[0-9]+\.[0-9]+\.[0-9]+"
            if platform == "linux"
            else r"libaoahid\.[0-9]+\.[0-9]+\.[0-9]+\.dylib"
        )
        with tarfile.open(path, "r:gz") as archive:
            matches = [
                member
                for member in archive.getmembers()
                if member.isfile()
                and re.fullmatch(
                    rf"{re.escape(root)}/lib/{runtime_pattern}",
                    member.name,
                )
            ]
            if len(matches) != 1:
                raise CollectionError(
                    f"{path.name}: expected exactly one regular versioned libaoahid shared library"
                )
            stream = archive.extractfile(matches[0])
            if stream is None:
                raise CollectionError(f"{path.name}: shared object is not extractable")
            return PurePosixPath(matches[0].name).name, stream.read()
    with zipfile.ZipFile(path) as archive:
        member = f"{root}/bin/aoahid.dll"
        try:
            info = archive.getinfo(member)
        except KeyError as error:
            raise CollectionError(f"{path.name}: aoahid.dll is missing") from error
        if info.is_dir():
            raise CollectionError(f"{path.name}: aoahid.dll is not a regular file")
        return "aoahid.dll", archive.read(member)


def validate_runtime_architecture(data: bytes, platform: str, arch: str) -> None:
    if platform == "linux":
        if len(data) < 20 or data[:4] != b"\x7fELF":
            raise CollectionError("direct Linux runtime is not an ELF object")
        if data[4] != 2 or data[5] != 1:
            raise CollectionError("direct Linux runtime is not little-endian ELF64")
        object_type, machine = struct.unpack_from("<HH", data, 16)
        expected_machine = {"x86_64": 62, "aarch64": 183}[arch]
        if object_type != 3 or machine != expected_machine:
            raise CollectionError(
                f"direct Linux runtime has ELF type/machine {object_type}/{machine}, "
                f"expected ET_DYN/{expected_machine}"
            )
        return

    if platform == "macos":
        # Thin little-endian Mach-O 64 dynamic library (MH_MAGIC_64, MH_DYLIB).
        if len(data) < 16 or data[:4] != b"\xcf\xfa\xed\xfe":
            raise CollectionError("direct macOS runtime is not a thin 64-bit Mach-O object")
        cpu_type, _, file_type = struct.unpack_from("<iiI", data, 4)
        expected_cpu = {"x86_64": 0x01000007, "arm64": 0x0100000C}[arch]
        if cpu_type != expected_cpu or file_type != 6:
            raise CollectionError(
                f"direct macOS runtime has Mach-O cpu/file type {cpu_type:#x}/{file_type}, "
                f"expected {expected_cpu:#x}/6 (MH_DYLIB)"
            )
        return

    if len(data) < 0x40 or data[:2] != b"MZ":
        raise CollectionError("direct Windows runtime has no DOS/PE header")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset > len(data) - 26 or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise CollectionError("direct Windows runtime has no valid PE signature")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    characteristics = struct.unpack_from("<H", data, pe_offset + 22)[0]
    optional_magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    expected_machine = {"x86_64": 0x8664, "arm64": 0xAA64}[arch]
    if machine != expected_machine or optional_magic != 0x20B or characteristics & 0x2000 == 0:
        raise CollectionError(
            "direct Windows runtime is not the expected PE32+ DLL architecture"
        )


def runtime_bundle_documents(platform: str) -> tuple[str, ...]:
    if platform == "windows":
        return RUNTIME_BUNDLE_DOCUMENTS + WINDOWS_RUNTIME_BUNDLE_DOCUMENTS
    return RUNTIME_BUNDLE_DOCUMENTS


def is_runtime_binary(relative: str, platform: str) -> bool:
    """Report whether one shared-archive member is a runtime binary."""
    parts = PurePosixPath(relative).parts
    if len(parts) != 2:
        return False
    if platform == "linux":
        return parts[0] == "lib" and ".so" in parts[1]
    if platform == "macos":
        return parts[0] == "lib" and parts[1].endswith(".dylib")
    return parts[0] == "bin" and parts[1].lower().endswith(".dll")


def build_runtime_bundle(
    directory: Path,
    archive: Path,
    version: str,
    platform: str,
    arch: str,
    qualifier: str,
    created_epoch: int,
) -> Path:
    """Repackage one shared archive as a redistributable runtime-only asset.

    Every member is copied byte-for-byte out of the already validated shared
    archive, so the bundled runtime is the same binary the complete archive
    ships and inherits its architecture and glibc validation.
    """
    _, runtime_contents = shared_runtime(archive, platform)
    validate_runtime_architecture(runtime_contents, platform, arch)
    root = archive.name.removesuffix(".tar.gz").removesuffix(".zip")
    bundle_root = (
        f"libaoahid-{version}-{platform_part(platform, arch, qualifier)}-runtime"
    )
    bundle = directory / runtime_bundle_name(version, platform, arch, qualifier)
    if bundle.exists():
        raise CollectionError(f"refusing to overwrite existing runtime bundle {bundle.name}")

    documents = runtime_bundle_documents(platform)
    members = archive_members(archive)
    missing = sorted(set(documents) - members)
    if missing:
        raise CollectionError(
            f"{archive.name}: runtime bundle sources are missing: {', '.join(missing)}"
        )
    binaries = sorted(name for name in members if is_runtime_binary(name, platform))
    if not binaries:
        raise CollectionError(f"{archive.name}: no runtime binary to bundle")

    if platform in TAR_PLATFORMS:
        with tarfile.open(archive, "r:gz") as source:
            with bundle.open("wb") as raw:
                with gzip.GzipFile(
                    filename="", mode="wb", fileobj=raw, mtime=created_epoch
                ) as compressed:
                    with tarfile.open(
                        mode="w", fileobj=compressed, format=tarfile.PAX_FORMAT
                    ) as output:
                        entries = [
                            (relative, source.getmember(f"{root}/{relative}"))
                            for relative in binaries + list(documents)
                        ]
                        for relative, member in entries:
                            copied = tarfile.TarInfo(f"{bundle_root}/{relative}")
                            copied.mtime = created_epoch
                            copied.uid = 0
                            copied.gid = 0
                            copied.uname = ""
                            copied.gname = ""
                            copied.mode = member.mode
                            copied.type = member.type
                            if member.issym():
                                copied.linkname = member.linkname
                                output.addfile(copied)
                                continue
                            payload = source.extractfile(member)
                            if payload is None:
                                raise CollectionError(
                                    f"{archive.name}: {relative} is not extractable"
                                )
                            data = payload.read()
                            copied.size = len(data)
                            output.addfile(copied, io.BytesIO(data))
                        readme = RUNTIME_BUNDLE_README.encode("utf-8")
                        info = tarfile.TarInfo(
                            f"{bundle_root}/{RUNTIME_BUNDLE_README_NAME}"
                        )
                        info.mtime = created_epoch
                        info.mode = 0o644
                        info.size = len(readme)
                        output.addfile(info, io.BytesIO(readme))
        return bundle

    stamp = datetime.fromtimestamp(max(created_epoch, 315532800), timezone.utc)
    date_time = (
        stamp.year,
        stamp.month,
        stamp.day,
        stamp.hour,
        stamp.minute,
        stamp.second,
    )
    with zipfile.ZipFile(archive) as source:
        with zipfile.ZipFile(
            bundle, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as output:
            payloads = [
                (relative, source.read(f"{root}/{relative}"))
                for relative in binaries + list(documents)
            ]
            payloads.append(
                (RUNTIME_BUNDLE_README_NAME, RUNTIME_BUNDLE_README.encode("utf-8"))
            )
            for relative, data in payloads:
                info = zipfile.ZipInfo(f"{bundle_root}/{relative}", date_time)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                output.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return bundle


def validate_runtime_bundle(
    bundle: Path,
    archive: Path,
    platform: str,
    arch: str,
) -> None:
    """Require the bundle to carry every license obligation of its binaries."""
    members = archive_members(bundle)
    documents = runtime_bundle_documents(platform)
    required = set(documents) | {RUNTIME_BUNDLE_README_NAME}
    missing = sorted(required - members)
    if missing:
        raise CollectionError(f"{bundle.name}: missing {', '.join(missing)}")
    binaries = sorted(name for name in members if is_runtime_binary(name, platform))
    if not binaries:
        raise CollectionError(f"{bundle.name}: contains no runtime binary")
    unexpected = sorted(members - required - set(binaries))
    if unexpected:
        raise CollectionError(f"{bundle.name}: unexpected member {', '.join(unexpected)}")
    if not any("usb" in PurePosixPath(name).name.lower() for name in binaries):
        raise CollectionError(
            f"{bundle.name}: the libusb runtime is missing from the bundle"
        )
    if (
        hashlib.sha256(archive_bytes(bundle, LIBUSB_SOURCE_NAME)).hexdigest()
        != LIBUSB_SOURCE_SHA256
    ):
        raise CollectionError(f"{bundle.name}: bundled libusb source has the wrong SHA-256")
    if (
        hashlib.sha256(archive_bytes(bundle, LIBUSB_LICENSE_NAME)).hexdigest()
        != LIBUSB_LICENSE_SHA256
    ):
        raise CollectionError(f"{bundle.name}: bundled libusb license has the wrong SHA-256")
    if platform == "windows" and (
        hashlib.sha256(archive_bytes(bundle, VCPKG_LICENSE_NAME)).hexdigest()
        != VCPKG_LICENSE_SHA256
    ):
        raise CollectionError(f"{bundle.name}: bundled vcpkg license has the wrong SHA-256")
    # Every bundled member must be the exact byte sequence already validated in
    # the complete archive; the bundle adds only its own README.
    bundle_links = archive_symlinks(bundle)
    archive_links = archive_symlinks(archive)
    for relative in sorted(members - {RUNTIME_BUNDLE_README_NAME}):
        if relative in bundle_links or relative in archive_links:
            if bundle_links.get(relative) != archive_links.get(relative):
                raise CollectionError(
                    f"{bundle.name}: {relative} is not the archive's symbolic link"
                )
            continue
        if archive_bytes(bundle, relative) != archive_bytes(archive, relative):
            raise CollectionError(
                f"{bundle.name}: {relative} differs from the complete archive"
            )
    _, runtime_contents = shared_runtime(archive, platform)
    validate_runtime_architecture(runtime_contents, platform, arch)


def require_layout(
    path: Path,
    platform: str,
    arch: str,
    variant: str,
    version: str,
    source_sha: str,
    created_epoch: int,
) -> tuple[str, str]:
    members = archive_members(path)
    required = {
        "include/aoahid.h",
        "include/aoahid.hpp",
        "lib/cmake/aoahid/aoahid-config.cmake",
        "lib/cmake/aoahid/aoahid-config-version.cmake",
        "lib/cmake/aoahid/aoahid-check-libusb.cmake",
        "lib/pkgconfig/aoahid.pc",
        "share/doc/libaoahid/LICENSE",
        "share/doc/libaoahid/NOTICE",
        "share/doc/libaoahid/CHANGELOG.md",
        "share/doc/libaoahid/THIRD_PARTY_NOTICES.md",
        "share/doc/libaoahid/build-metadata.json",
        LIBUSB_LICENSE_NAME,
        LIBUSB_SOURCE_NAME,
        "share/doc/libaoahid/libaoahid.spdx.json",
    }
    if platform == "windows":
        required.add("share/doc/libaoahid/third-party/vcpkg-libusb-port/vcpkg.json")
        required.add("share/doc/libaoahid/third-party/vcpkg-libusb-port/portfile.cmake")
        required.add(VCPKG_LICENSE_NAME)
    if variant == "static":
        required.add("lib/pkgconfig/libusb-1.0.pc")
    if platform == "linux":
        required.add("share/doc/libaoahid/glibc-requirements.json")
    missing = sorted(required - members)
    if missing:
        raise CollectionError(f"{path.name}: missing {', '.join(missing)}")
    if hashlib.sha256(archive_bytes(path, LIBUSB_SOURCE_NAME)).hexdigest() != LIBUSB_SOURCE_SHA256:
        raise CollectionError(f"{path.name}: bundled libusb source archive has the wrong SHA-256")
    if hashlib.sha256(archive_bytes(path, LIBUSB_LICENSE_NAME)).hexdigest() != LIBUSB_LICENSE_SHA256:
        raise CollectionError(f"{path.name}: bundled libusb license text has the wrong SHA-256")
    if platform == "windows" and (
        hashlib.sha256(archive_bytes(path, VCPKG_LICENSE_NAME)).hexdigest()
        != VCPKG_LICENSE_SHA256
    ):
        raise CollectionError(f"{path.name}: bundled vcpkg license has the wrong SHA-256")
    if platform == "windows":
        for relative, expected_sha256 in generate_spdx.VCPKG_FILE_SHA256.items():
            if hashlib.sha256(archive_bytes(path, relative)).hexdigest() != expected_sha256:
                raise CollectionError(
                    f"{path.name}: bundled pinned vcpkg material changed: {relative}"
                )
    component = "shared" if variant == "shared" else "static"
    other_component = "static" if variant == "shared" else "shared"
    target_export = f"lib/cmake/aoahid/aoahid-{component}-targets.cmake"
    target_configs = {
        name
        for name in members
        if re.fullmatch(
            rf"lib/cmake/aoahid/aoahid-{component}-targets-[^/]+\.cmake",
            name,
        )
    }
    unexpected_exports = {
        name
        for name in members
        if re.fullmatch(
            rf"lib/cmake/aoahid/aoahid-{other_component}-targets(?:-[^/]+)?\.cmake",
            name,
        )
    }
    if target_export not in members or len(target_configs) != 1 or unexpected_exports:
        raise CollectionError(
            f"{path.name}: incomplete or mixed CMake {component} target exports"
        )
    try:
        pkg_config_metadata.validate_relocatable(
            archive_bytes(path, "lib/pkgconfig/aoahid.pc"),
            label=f"{path.name}: aoahid.pc",
            expected_name="libaoahid",
            expected_version=version,
            expected_url=package_source.REPOSITORY_URL,
            expected_library=(
                "aoahid_static"
                if platform == "windows" and variant == "static"
                else "aoahid"
            ),
            expected_requires_private=(
                "libusb-1.0 >= 1.0.30" if variant == "static" else None
            ),
            expected_private_libraries=(
                ("-lstdc++", "-lc++")
                if variant == "static" and platform in ("linux", "macos")
                else None
            ),
        )
        if variant == "static":
            pkg_config_metadata.validate_relocatable(
                archive_bytes(path, "lib/pkgconfig/libusb-1.0.pc"),
                label=f"{path.name}: libusb-1.0.pc",
                expected_name="libusb-1.0",
                expected_version="1.0.30",
                expected_library=("libusb-1.0" if platform == "windows" else "usb-1.0"),
            )
    except (KeyError, pkg_config_metadata.PkgConfigError) as error:
        raise CollectionError(f"{path.name}: invalid pkg-config metadata: {error}") from error
    if platform == "linux" and variant == "shared":
        okay = (
            any(name.startswith("lib/libaoahid.so.") for name in members)
            and sum(
                name.startswith("lib/libusb-1.0.so.0.")
                and name.count(".") >= 5
                for name in members
            )
            == 1
        )
    elif platform == "linux":
        okay = (
            "lib/libaoahid.a" in members
            and "lib/libusb-1.0.so" in members
            and "include/libusb-1.0/libusb.h" in members
            and sum(
                name.startswith("lib/libusb-1.0.so.0.")
                and name.count(".") >= 5
                for name in members
            )
            == 1
        )
    elif platform == "macos" and variant == "shared":
        okay = (
            any(
                re.fullmatch(r"lib/libaoahid\.[0-9]+\.[0-9]+\.[0-9]+\.dylib", name)
                for name in members
            )
            and "lib/libusb-1.0.0.dylib" in members
            and not any(name.endswith(".a") for name in members)
        )
    elif platform == "macos":
        okay = (
            "lib/libaoahid.a" in members
            and "lib/libusb-1.0.dylib" in members
            and "lib/libusb-1.0.0.dylib" in members
            and "include/libusb-1.0/libusb.h" in members
            and not any(name.startswith("lib/libaoahid") and name.endswith(".dylib") for name in members)
        )
    elif variant == "shared":
        dependency_dlls = {
            name
            for name in members
            if name.startswith("bin/")
            and "usb" in name.lower()
            and name.endswith(".dll")
        }
        dependency_imports = {
            name
            for name in members
            if name.startswith("lib/dependencies/")
            and "usb" in name.lower()
            and name.endswith(".lib")
        }
        okay = (
            {"bin/aoahid.dll", "lib/aoahid.lib"}.issubset(members)
            and len(dependency_dlls) == 1
            and len(dependency_imports) == 1
        )
    else:
        dependency_dlls = {
            name
            for name in members
            if name.startswith("bin/")
            and "usb" in name.lower()
            and name.endswith(".dll")
        }
        dependency_link_libraries = {
            name
            for name in members
            if name.startswith("lib/")
            and name.count("/") == 1
            and "usb" in name.lower()
            and name.endswith(".lib")
        }
        dependency_imports = {
            name
            for name in members
            if name.startswith("lib/dependencies/")
            and "usb" in name.lower()
            and name.endswith(".lib")
        }
        okay = (
            "lib/aoahid_static.lib" in members
            and "include/libusb-1.0/libusb.h" in members
            and len(dependency_dlls) == 1
            and len(dependency_link_libraries) == 1
            and len(dependency_imports) == 1
        )
    if not okay:
        raise CollectionError(f"{path.name}: expected {platform}/{variant} library files are missing")
    if platform == "macos":
        validate_runtime_architecture(
            archive_bytes(path, "lib/libusb-1.0.0.dylib"), platform, arch
        )

    try:
        metadata = json.loads(archive_bytes(path, "share/doc/libaoahid/build-metadata.json"))
    except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CollectionError(f"{path.name}: invalid build metadata: {error}") from error
    if not isinstance(metadata, dict):
        raise CollectionError(f"{path.name}: build metadata is not an object")
    expected = {
        "schema": 1,
        "name": "libaoahid",
        "version": version,
        "platform": platform,
        "architecture": arch,
        "variant": variant,
        "repository": package_source.REPOSITORY_URL,
        "release_download_url": generate_spdx.release_download_url(
            version, platform, arch, variant
        ),
        "source_commit": source_sha,
        "source_date_epoch": created_epoch,
    }
    differing = {key: (metadata.get(key), value) for key, value in expected.items() if metadata.get(key) != value}
    if differing:
        raise CollectionError(f"{path.name}: build metadata mismatch: {differing}")
    libusb_version = metadata.get("libusb_version")
    if not isinstance(libusb_version, str):
        raise CollectionError(f"{path.name}: libusb_version is missing from build metadata")
    if libusb_version != "1.0.30":
        raise CollectionError(f"{path.name}: dependency version does not match bundled source")
    glibc_maximum_required = ""
    if platform == "linux":
        try:
            glibc_document = json.loads(
                archive_bytes(path, "share/doc/libaoahid/glibc-requirements.json")
            )
        except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CollectionError(
                f"{path.name}: invalid glibc requirements document: {error}"
            ) from error
        root = path.name.removesuffix(".tar.gz")
        with tarfile.open(path, "r:gz") as archive:
            expected_glibc_paths = {
                PurePosixPath(member.name).relative_to(root).as_posix()
                for member in archive.getmembers()
                if member.isfile()
                and PurePosixPath(member.name).parent == PurePosixPath(root, "lib")
                and ".so" in PurePosixPath(member.name).name
            }
        try:
            glibc_maximum_required = glibc_versions.validate_document(
                glibc_document,
                expected_paths=expected_glibc_paths,
            )
        except glibc_versions.GlibcVersionError as error:
            raise CollectionError(f"{path.name}: {error}") from error
        if (
            metadata.get("glibc_baseline") != glibc_versions.BASELINE
            or metadata.get("glibc_maximum_required") != glibc_maximum_required
        ):
            raise CollectionError(
                f"{path.name}: build metadata has inconsistent glibc requirements"
            )
    return libusb_version, glibc_maximum_required


def validate_sidecar(
    path: Path,
    archive: Path,
    version: str,
    platform: str,
    arch: str,
    variant: str,
    source_sha: str,
    created_epoch: int,
    libusb_version: str,
) -> str:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CollectionError(f"invalid SPDX sidecar {path.name}: {error}") from error
    if document.get("spdxVersion") != "SPDX-2.3" or document.get("dataLicense") != "CC0-1.0":
        raise CollectionError(f"{path.name}: not an SPDX-2.3 document")
    expected_name = f"libaoahid-{version}-{platform}-{arch}-{variant}"
    if document.get("name") != expected_name:
        raise CollectionError(f"{path.name}: SPDX name is not {expected_name}")
    packages = document.get("packages", [])
    if not isinstance(packages, list):
        raise CollectionError(f"{path.name}: SPDX packages list is invalid")
    own = next((p for p in packages if isinstance(p, dict) and p.get("name") == "libaoahid"), None)
    if (
        own is None
        or own.get("SPDXID") != "SPDXRef-Package-libaoahid"
        or own.get("versionInfo") != version
        or own.get("filesAnalyzed") is not True
    ):
        raise CollectionError(f"{path.name}: incomplete libaoahid package record")
    if own.get("sourceInfo") != f"Git commit {source_sha}":
        raise CollectionError(f"{path.name}: SPDX source commit is inconsistent")
    if own.get("homepage") != package_source.REPOSITORY_URL:
        raise CollectionError(f"{path.name}: SPDX homepage is not the canonical repository")
    expected_download = generate_spdx.release_download_url(
        version, platform, arch, variant
    )
    if own.get("downloadLocation") != expected_download:
        raise CollectionError(
            f"{path.name}: SPDX download location is not the exact release asset"
        )
    if own.get("packageFileName") != archive.name:
        raise CollectionError(f"{path.name}: SPDX package file name is inconsistent")
    dependency = next(
        (p for p in packages if isinstance(p, dict) and p.get("name") == "libusb"),
        None,
    )
    if dependency is None or dependency.get("SPDXID") != "SPDXRef-Package-libusb":
        raise CollectionError(f"{path.name}: libusb dependency record is missing")
    if dependency.get("versionInfo") != libusb_version:
        raise CollectionError(f"{path.name}: SPDX libusb version is inconsistent")
    if dependency.get("packageFileName") != "libusb-1.0.30.tar.bz2":
        raise CollectionError(f"{path.name}: SPDX libusb source-package name is inconsistent")
    dependency_checksums = {
        item.get("algorithm"): item.get("checksumValue")
        for item in dependency.get("checksums", [])
        if isinstance(item, dict)
    }
    if dependency_checksums.get("SHA256") != LIBUSB_SOURCE_SHA256:
        raise CollectionError(f"{path.name}: SPDX libusb source-package checksum is inconsistent")
    vcpkg = next(
        (p for p in packages if isinstance(p, dict) and p.get("name") == "vcpkg"),
        None,
    )
    vcpkg_files = (
        VCPKG_LICENSE_NAME,
        "share/doc/libaoahid/third-party/vcpkg-libusb-port/portfile.cmake",
        "share/doc/libaoahid/third-party/vcpkg-libusb-port/vcpkg.json",
    )
    if platform == "windows":
        if len(packages) != 3:
            raise CollectionError(f"{path.name}: Windows SPDX must contain three packages")
        if (
            not isinstance(vcpkg, dict)
            or vcpkg.get("SPDXID") != "SPDXRef-Package-vcpkg"
            or vcpkg.get("versionInfo") != "ddd0023b0eee70986e42ed49d9d4afb8098f212e"
            or vcpkg.get("filesAnalyzed") is not True
            or vcpkg.get("licenseConcluded") != "MIT"
            or vcpkg.get("licenseDeclared") != "MIT"
        ):
            raise CollectionError(f"{path.name}: SPDX vcpkg build-material record is missing")
        vcpkg_sha1 = sorted(
            hashlib.sha1(archive_bytes(archive, relative)).hexdigest()
            for relative in vcpkg_files
        )
        expected_vcpkg_code = hashlib.sha1("".join(vcpkg_sha1).encode("ascii")).hexdigest()
        vcpkg_code = vcpkg.get("packageVerificationCode")
        if (
            not isinstance(vcpkg_code, dict)
            or vcpkg_code.get("packageVerificationCodeValue") != expected_vcpkg_code
        ):
            raise CollectionError(f"{path.name}: SPDX vcpkg verification code is inconsistent")
    elif vcpkg is not None or len(packages) != 2:
        raise CollectionError(
            f"{path.name}: a non-Windows SPDX must contain only libaoahid and libusb"
        )
    created = datetime.fromtimestamp(created_epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    creation_info = document.get("creationInfo")
    if not isinstance(creation_info, dict) or creation_info.get("created") != created:
        raise CollectionError(f"{path.name}: SPDX creation time is inconsistent")

    members = archive_members(archive)
    expected_files = members - {"share/doc/libaoahid/libaoahid.spdx.json"}
    listed = document.get("files")
    if not isinstance(listed, list) or not all(isinstance(item, dict) for item in listed):
        raise CollectionError(f"{path.name}: SPDX files list is invalid")
    listed_names = [str(item.get("fileName", "")) for item in listed]
    if not all(name.startswith("./") for name in listed_names):
        raise CollectionError(f"{path.name}: SPDX file names must start with ./")
    normalized_names = [name.removeprefix("./") for name in listed_names]
    if len(normalized_names) != len(set(normalized_names)) or set(normalized_names) != expected_files:
        raise CollectionError(f"{path.name}: SPDX file list differs from archive contents")
    sha1_values = []
    expected_file_ids: set[str] = set()
    observed_licenses: set[str] = set()
    for item, relative in zip(listed, normalized_names):
        expected_identifier = generate_spdx.file_id(relative)
        if item.get("SPDXID") != expected_identifier:
            raise CollectionError(
                f"{path.name}: SPDX identifier is inconsistent for {relative}"
            )
        expected_file_ids.add(expected_identifier)
        contents = archive_bytes(archive, relative)
        expected_checksums = {
            "SHA1": hashlib.sha1(contents).hexdigest(),
            "SHA256": hashlib.sha256(contents).hexdigest(),
        }
        checksum_entries = item.get("checksums")
        if not isinstance(checksum_entries, list):
            raise CollectionError(f"{path.name}: SPDX checksums are invalid for {relative}")
        checksums = {
            value.get("algorithm"): value.get("checksumValue")
            for value in checksum_entries
            if isinstance(value, dict)
        }
        if (
            checksums.get("SHA1") != expected_checksums["SHA1"]
            or checksums.get("SHA256") != expected_checksums["SHA256"]
        ):
            raise CollectionError(f"{path.name}: SPDX checksum mismatch for {relative}")
        license_expression, copyright_text = generate_spdx.file_license(relative)
        observed_licenses.add(license_expression)
        if (
            item.get("licenseConcluded") != license_expression
            or item.get("licenseInfoInFiles") != [license_expression]
            or item.get("copyrightText") != copyright_text
        ):
            raise CollectionError(
                f"{path.name}: SPDX file-level license is inconsistent for {relative}"
            )
        sha1_values.append(expected_checksums["SHA1"])
    if (
        own.get("licenseDeclared") != generate_spdx.MIT_LICENSE
        or own.get("licenseConcluded") != " AND ".join(sorted(observed_licenses))
        or own.get("licenseInfoFromFiles") != sorted(observed_licenses)
    ):
        raise CollectionError(
            f"{path.name}: SPDX package license summary disagrees with its files"
        )
    if generate_spdx.LIBUSB_LICENSE not in observed_licenses:
        raise CollectionError(
            f"{path.name}: no packaged file is attributed to the bundled libusb runtime"
        )
    verification = hashlib.sha1("".join(sorted(sha1_values)).encode("ascii")).hexdigest()
    code = own.get("packageVerificationCode")
    if not isinstance(code, dict) or code.get("packageVerificationCodeValue") != verification:
        raise CollectionError(f"{path.name}: SPDX package verification code is inconsistent")

    relationships = document.get("relationships")
    if not isinstance(relationships, list):
        raise CollectionError(f"{path.name}: SPDX relationships list is invalid")
    if document.get("documentDescribes") != ["SPDXRef-Package-libaoahid"]:
        raise CollectionError(f"{path.name}: SPDX documentDescribes is inconsistent")

    expected_relationships = {
        (
            "SPDXRef-DOCUMENT",
            "DESCRIBES",
            "SPDXRef-Package-libaoahid",
        ),
        (
            "SPDXRef-Package-libaoahid",
            "DEPENDS_ON",
            "SPDXRef-Package-libusb",
        ),
    }
    expected_relationships.update(
        ("SPDXRef-Package-libaoahid", "CONTAINS", identifier)
        for identifier in expected_file_ids
    )
    if platform == "windows":
        expected_vcpkg_ids = {
            generate_spdx.file_id(relative) for relative in vcpkg_files
        }
        expected_relationships.add(
            (
                "SPDXRef-Package-vcpkg",
                "BUILD_DEPENDENCY_OF",
                "SPDXRef-Package-libaoahid",
            )
        )
        expected_relationships.update(
            ("SPDXRef-Package-vcpkg", "CONTAINS", identifier)
            for identifier in expected_vcpkg_ids
        )
    actual_relationships: list[tuple[str, str, str]] = []
    for item in relationships:
        if not isinstance(item, dict):
            raise CollectionError(f"{path.name}: SPDX relationship is not an object")
        element = item.get("spdxElementId")
        relation = item.get("relationshipType")
        related = item.get("relatedSpdxElement")
        if not all(isinstance(value, str) for value in (element, relation, related)):
            raise CollectionError(f"{path.name}: SPDX relationship is incomplete")
        actual_relationships.append((element, relation, related))
    if (
        len(actual_relationships) != len(set(actual_relationships))
        or set(actual_relationships) != expected_relationships
    ):
        raise CollectionError(
            f"{path.name}: SPDX relationship graph is incomplete or contains extras"
        )
    namespace = document.get("documentNamespace")
    if not isinstance(namespace, str):
        raise CollectionError(f"{path.name}: documentNamespace is missing")
    return namespace


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=Path("."))
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--created-epoch", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version) is None:
            raise CollectionError("version must have the exact form MAJOR.MINOR.PATCH")
        if re.fullmatch(r"[0-9a-f]{40}", args.source_sha) is None:
            raise CollectionError("source SHA must be 40 lowercase hexadecimal characters")
        records = []
        expected: set[str] = set()
        source_archive = args.directory / package_source.source_archive_name(args.version)
        if not source_archive.is_file():
            raise CollectionError(
                f"required source archive is missing: {source_archive.name}"
            )
        try:
            package_source.validate_source_archive(
                source_archive,
                args.source_root,
                args.version,
                args.source_sha,
                args.created_epoch,
            )
        except package_source.SourcePackageError as error:
            raise CollectionError(f"invalid source archive: {error}") from error
        expected.add(source_archive.name)
        for platform, arch, qualifier, extension in TARGETS:
            platform_part = f"{platform}-{arch}" + (f"-{qualifier}" if qualifier else "")
            for variant in VARIANTS:
                base = f"libaoahid-{args.version}-{platform_part}-{variant}"
                archive = args.directory / (base + extension)
                sidecar = args.directory / (base + ".spdx.json")
                expected.update((archive.name, sidecar.name))
                if not archive.is_file() or not sidecar.is_file():
                    raise CollectionError(f"required artifact pair is missing: {base}")
                libusb_version, glibc_maximum_required = require_layout(
                    archive,
                    platform,
                    arch,
                    variant,
                    args.version,
                    args.source_sha,
                    args.created_epoch,
                )
                namespace = validate_sidecar(
                    sidecar,
                    archive,
                    args.version,
                    platform,
                    arch,
                    variant,
                    args.source_sha,
                    args.created_epoch,
                    libusb_version,
                )
                internal_spdx = archive_bytes(archive, "share/doc/libaoahid/libaoahid.spdx.json")
                if internal_spdx != sidecar.read_bytes():
                    raise CollectionError(f"{archive.name}: embedded and sidecar SPDX documents differ")
                records.append(
                    record := {
                        "platform": platform,
                        "architecture": arch,
                        "qualifier": qualifier or None,
                        "variant": variant,
                        "archive": archive.name,
                        "archive_sha256": sha256(archive),
                        "spdx": sidecar.name,
                        "spdx_sha256": sha256(sidecar),
                        "spdx_namespace": namespace,
                    }
                )
                if platform == "linux":
                    record["glibc_baseline"] = glibc_versions.BASELINE
                    record["glibc_maximum_required"] = glibc_maximum_required
                if variant == "shared":
                    runtime = build_runtime_bundle(
                        args.directory,
                        archive,
                        args.version,
                        platform,
                        arch,
                        qualifier,
                        args.created_epoch,
                    )
                    validate_runtime_bundle(runtime, archive, platform, arch)
                    expected.add(runtime.name)
                    record["runtime_bundle"] = runtime.name
                    record["runtime_bundle_sha256"] = sha256(runtime)
                    if platform == "linux":
                        record["runtime_glibc_maximum_required"] = glibc_maximum_required
        expected_uploads = expected_release_asset_names(args.version)
        if expected != expected_uploads - {"release-manifest.json", "SHA256SUMS"}:
            raise CollectionError("internal release asset-name policy is inconsistent")
        present = {
            path.name
            for path in args.directory.iterdir()
            if path.is_file() and (path.name.startswith("libaoahid-") or path.suffix in (".zip", ".gz"))
        }
        extras = sorted(present - expected)
        if extras:
            raise CollectionError("unexpected release artifacts: " + ", ".join(extras))

        manifest = {
            "schema": 1,
            "name": "libaoahid",
            "version": args.version,
            "repository": package_source.REPOSITORY_URL,
            "release_tag": f"v{args.version}",
            "source_commit": args.source_sha,
            "created": datetime.fromtimestamp(args.created_epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "source": {
                "archive": source_archive.name,
                "archive_sha256": sha256(source_archive),
                "download_url": package_source.source_download_url(args.version),
            },
            "targets": records,
        }
        manifest_path = args.directory / "release-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        checksum_files = sorted([args.directory / name for name in expected] + [manifest_path], key=lambda p: p.name)
        (args.directory / "SHA256SUMS").write_text(
            "".join(f"{sha256(path)}  {path.name}\n" for path in checksum_files), encoding="utf-8"
        )
    except (OSError, ValueError, CollectionError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"collect-release: error: {error}", file=sys.stderr)
        return 1
    print(
        f"collect-release: validated {len(TARGETS) * len(VARIANTS)} packages, 1 source archive, "
        f"and {len(TARGETS)} runtime bundles; "
        "wrote release-manifest.json and SHA256SUMS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
