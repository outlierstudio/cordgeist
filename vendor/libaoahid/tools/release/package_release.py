#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Create one deterministic binary distribution and its SPDX sidecar."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import platform as runtime_platform
import re
import shutil
import stat
import sys
import tarfile
import tempfile
import zipfile

import generate_spdx
import glibc_versions
import package_source
import pkg_config_metadata


class PackageError(RuntimeError):
    pass


def validate_arguments(args: argparse.Namespace) -> None:
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version) is None:
        raise PackageError("version must have the exact form MAJOR.MINOR.PATCH")
    if re.fullmatch(r"[0-9a-f]{40}", args.source_sha) is None:
        raise PackageError("source SHA must be 40 lowercase hexadecimal characters")
    allowed_architectures = {
        "linux": {"x86_64", "aarch64"},
        "macos": {"x86_64", "arm64"},
        "windows": {"x86_64", "arm64"},
    }
    if args.arch not in allowed_architectures[args.platform]:
        raise PackageError(f"unsupported platform/architecture pair: {args.platform}/{args.arch}")
    expected_base_name = generate_spdx.release_archive_name(
        args.version, args.platform, args.arch, args.variant
    ).removesuffix(".tar.gz").removesuffix(".zip")
    if args.base_name != expected_base_name:
        raise PackageError(
            f"base name is {args.base_name!r}, expected exact release name "
            f"{expected_base_name!r}"
        )


def require_one(root: Path, patterns: tuple[str, ...], label: str) -> Path:
    matches = sorted({path for pattern in patterns for path in root.glob(pattern) if path.is_file()})
    if len(matches) != 1:
        rendered = ", ".join(path.relative_to(root).as_posix() for path in matches) or "none"
        raise PackageError(f"expected exactly one {label}; found {rendered}")
    return matches[0]


def validate_layout(root: Path, platform: str, variant: str, version: str) -> None:
    for relative in (
        "include/aoahid.h",
        "include/aoahid.hpp",
        "share/doc/libaoahid/LICENSE",
        "share/doc/libaoahid/NOTICE",
        "share/doc/libaoahid/CHANGELOG.md",
        "share/doc/libaoahid/THIRD_PARTY_NOTICES.md",
        "share/doc/libaoahid/build-metadata.json",
        "share/doc/libaoahid/third-party/libusb-copyright",
        "share/doc/libaoahid/third-party/source/libusb-1.0.30.tar.bz2",
        "lib/cmake/aoahid/aoahid-check-libusb.cmake",
    ):
        if not (root / relative).is_file():
            raise PackageError(f"required package file is missing: {relative}")
    require_one(root, ("lib/cmake/aoahid/aoahid-config.cmake",), "CMake package config")
    require_one(
        root,
        ("lib/cmake/aoahid/aoahid-config-version.cmake",),
        "CMake package version config",
    )
    component = "shared" if variant == "shared" else "static"
    other_component = "static" if variant == "shared" else "shared"
    require_one(
        root,
        (f"lib/cmake/aoahid/aoahid-{component}-targets.cmake",),
        f"CMake {component} target export",
    )
    require_one(
        root,
        (f"lib/cmake/aoahid/aoahid-{component}-targets-*.cmake",),
        f"configuration-specific CMake {component} target export",
    )
    if list(root.glob(f"lib/cmake/aoahid/aoahid-{other_component}-targets*.cmake")):
        raise PackageError(f"{variant} package contains the {other_component} CMake target export")
    aoahid_pc = require_one(root, ("lib/pkgconfig/aoahid.pc",), "aoahid pkg-config metadata")
    pkg_config_metadata.validate_relocatable(
        aoahid_pc.read_bytes(),
        label="aoahid.pc",
        expected_name="libaoahid",
        expected_version=version,
        expected_url=package_source.REPOSITORY_URL,
        expected_library=(
            "aoahid_static" if platform == "windows" and variant == "static" else "aoahid"
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
    source_archive = root / generate_spdx.LIBUSB_SOURCE_NAME
    if hashlib.sha256(source_archive.read_bytes()).hexdigest() != generate_spdx.LIBUSB_SOURCE_SHA256:
        raise PackageError("bundled libusb source archive has the wrong SHA-256")
    license_text = root / generate_spdx.LIBUSB_LICENSE_NAME
    if hashlib.sha256(license_text.read_bytes()).hexdigest() != generate_spdx.LIBUSB_LICENSE_SHA256:
        raise PackageError("bundled libusb license text has the wrong SHA-256")
    if platform == "linux" and variant == "shared":
        require_one(root, ("lib/libaoahid.so.*.*",), "versioned Linux shared object")
        require_one(root, ("lib/libusb-1.0.so.0.*.*",), "versioned libusb runtime")
        if list(root.glob("lib/*.a")):
            raise PackageError("shared package contains a static archive")
    elif platform == "linux":
        require_one(root, ("lib/libaoahid.a",), "Linux static archive")
        require_one(root, ("lib/libusb-1.0.so",), "linkable libusb shared library")
        require_one(root, ("lib/libusb-1.0.so.0.*.*",), "versioned libusb runtime")
        require_one(root, ("include/libusb-1.0/libusb.h",), "libusb public header")
        libusb_pc = require_one(
            root,
            ("lib/pkgconfig/libusb-1.0.pc",),
            "bundled libusb pkg-config metadata",
        )
        pkg_config_metadata.validate_relocatable(
            libusb_pc.read_bytes(),
            label="libusb-1.0.pc",
            expected_name="libusb-1.0",
            expected_version="1.0.30",
            expected_library="usb-1.0",
        )
        if list(root.glob("lib/libaoahid.so*")):
            raise PackageError("static package contains the libaoahid shared object")
    elif platform == "macos" and variant == "shared":
        require_one(root, ("lib/libaoahid.*.*.*.dylib",), "versioned macOS shared library")
        require_one(root, ("lib/libusb-1.0.0.dylib",), "libusb runtime")
        if list(root.glob("lib/*.a")):
            raise PackageError("shared package contains a static archive")
    elif platform == "macos":
        require_one(root, ("lib/libaoahid.a",), "macOS static archive")
        require_one(root, ("lib/libusb-1.0.dylib",), "linkable libusb shared library")
        require_one(root, ("lib/libusb-1.0.0.dylib",), "libusb runtime")
        require_one(root, ("include/libusb-1.0/libusb.h",), "libusb public header")
        libusb_pc = require_one(
            root,
            ("lib/pkgconfig/libusb-1.0.pc",),
            "bundled libusb pkg-config metadata",
        )
        pkg_config_metadata.validate_relocatable(
            libusb_pc.read_bytes(),
            label="libusb-1.0.pc",
            expected_name="libusb-1.0",
            expected_version="1.0.30",
            expected_library="usb-1.0",
        )
        if list(root.glob("lib/libaoahid*.dylib")):
            raise PackageError("static package contains the libaoahid shared library")
    elif variant == "shared":
        require_one(root, ("bin/aoahid.dll",), "Windows DLL")
        require_one(root, ("lib/aoahid.lib",), "Windows import library")
        require_one(root, ("bin/*usb*.dll",), "bundled libusb DLL")
        require_one(
            root,
            ("lib/dependencies/*usb*.lib",),
            "bundled libusb import library",
        )
        if list(root.glob("lib/aoahid_static.lib")):
            raise PackageError("shared package contains the aoahid static archive")
    else:
        require_one(root, ("lib/aoahid_static.lib",), "Windows static archive")
        require_one(root, ("bin/*usb*.dll",), "bundled libusb DLL")
        require_one(root, ("lib/*usb*.lib",), "linkable libusb import library")
        require_one(
            root,
            ("lib/dependencies/*usb*.lib",),
            "bundled libusb import library",
        )
        require_one(root, ("include/libusb-1.0/libusb.h",), "libusb public header")
        if list(root.glob("bin/aoahid.dll")):
            raise PackageError("static package contains the aoahid DLL")
        libusb_pc = require_one(
            root,
            ("lib/pkgconfig/libusb-1.0.pc",),
            "bundled libusb pkg-config metadata",
        )
        pkg_config_metadata.validate_relocatable(
            libusb_pc.read_bytes(),
            label="libusb-1.0.pc",
            expected_name="libusb-1.0",
            expected_version="1.0.30",
            expected_library="libusb-1.0",
        )
    if platform == "windows":
        vcpkg_manifest = require_one(
            root,
            ("share/doc/libaoahid/third-party/vcpkg-libusb-port/vcpkg.json",),
            "pinned vcpkg libusb port manifest",
        )
        vcpkg_portfile = require_one(
            root,
            ("share/doc/libaoahid/third-party/vcpkg-libusb-port/portfile.cmake",),
            "pinned vcpkg libusb portfile",
        )
        vcpkg_license = require_one(
            root,
            ("share/doc/libaoahid/third-party/vcpkg-LICENSE.txt",),
            "pinned vcpkg license",
        )
        if hashlib.sha256(vcpkg_license.read_bytes()).hexdigest() != generate_spdx.VCPKG_LICENSE_SHA256:
            raise PackageError("bundled vcpkg license has the wrong SHA-256")
        for vcpkg_file in (vcpkg_manifest, vcpkg_portfile):
            relative = vcpkg_file.relative_to(root).as_posix()
            if (
                hashlib.sha256(vcpkg_file.read_bytes()).hexdigest()
                != generate_spdx.VCPKG_FILE_SHA256[relative]
            ):
                raise PackageError(f"bundled pinned vcpkg material changed: {relative}")
    elif platform == "linux":
        glibc_path = require_one(
            root,
            ("share/doc/libaoahid/glibc-requirements.json",),
            "glibc requirements document",
        )
        try:
            glibc_document = json.loads(glibc_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise PackageError(f"glibc requirements document is invalid JSON: {error}") from error
        expected_glibc_paths = {
            path.relative_to(root).as_posix()
            for path in root.glob("lib/*.so*")
            if path.is_file() and not path.is_symlink()
        }
        maximum_required = glibc_versions.validate_document(
            glibc_document,
            expected_paths=expected_glibc_paths,
        )
        try:
            package_metadata = json.loads(
                (root / "share/doc/libaoahid/build-metadata.json").read_text(
                    encoding="utf-8"
                )
            )
        except json.JSONDecodeError as error:
            raise PackageError(f"build metadata is invalid JSON: {error}") from error
        if (
            package_metadata.get("glibc_baseline") != glibc_versions.BASELINE
            or package_metadata.get("glibc_maximum_required") != maximum_required
        ):
            raise PackageError("build metadata does not record the inspected glibc baseline")


def metadata(args: argparse.Namespace) -> dict[str, object]:
    result: dict[str, object] = {
        "schema": 1,
        "name": "libaoahid",
        "version": args.version,
        "platform": args.platform,
        "architecture": args.arch,
        "variant": args.variant,
        "source_commit": args.source_sha,
        "source_date_epoch": args.created_epoch,
        "libusb_version": args.libusb_version,
        "repository": package_source.REPOSITORY_URL,
        "release_download_url": generate_spdx.release_download_url(
            args.version, args.platform, args.arch, args.variant
        ),
        "packager_python": runtime_platform.python_version(),
    }
    for item in args.build_metadata:
        key, separator, value = item.partition("=")
        if not separator or not key:
            raise PackageError(f"invalid --build-metadata value: {item!r}")
        if key in result:
            raise PackageError(f"--build-metadata cannot replace reserved key {key!r}")
        result[key] = value
    return result


def write_tar_gz(package_root: Path, archive: Path, epoch: int) -> None:
    def normalize(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mtime = epoch
        return info

    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(mode="w", fileobj=compressed, format=tarfile.PAX_FORMAT) as tar:
                tar.add(package_root, arcname=package_root.name, recursive=True, filter=normalize)


def write_zip(package_root: Path, archive: Path, epoch: int) -> None:
    stamp = datetime.fromtimestamp(max(epoch, 315532800), timezone.utc)
    date_time = (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second)
    entries = [package_root] + sorted(package_root.rglob("*"), key=lambda p: p.relative_to(package_root.parent).as_posix())
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in entries:
            relative = path.relative_to(package_root.parent).as_posix()
            is_dir = path.is_dir()
            name = relative + ("/" if is_dir and not relative.endswith("/") else "")
            info = zipfile.ZipInfo(name, date_time)
            info.create_system = 3
            mode = 0o755 if is_dir or (path.stat().st_mode & stat.S_IXUSR) else 0o644
            file_type = stat.S_IFDIR if is_dir else stat.S_IFREG
            info.external_attr = (file_type | mode) << 16
            if is_dir:
                output.writestr(info, b"")
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
                output.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--base-name", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", choices=("linux", "macos", "windows"), required=True)
    parser.add_argument("--arch", choices=("x86_64", "aarch64", "arm64"), required=True)
    parser.add_argument("--variant", choices=("shared", "static"), required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--created-epoch", type=int, required=True)
    parser.add_argument("--libusb-version", required=True)
    parser.add_argument("--build-metadata", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        validate_arguments(args)
        if not args.stage.is_dir():
            raise PackageError(f"install stage does not exist: {args.stage}")
        args.out_dir.mkdir(parents=True, exist_ok=True)
        extension = ".zip" if args.platform == "windows" else ".tar.gz"
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.base_name) is None:
            raise PackageError("base name contains a path separator or unsupported character")
        archive = args.out_dir / (args.base_name + extension)
        sidecar = args.out_dir / (args.base_name + ".spdx.json")
        if archive.exists() or sidecar.exists():
            raise PackageError("refusing to overwrite an existing release artifact")

        with tempfile.TemporaryDirectory(prefix="libaoahid-package-") as temporary:
            package_root = Path(temporary) / args.base_name
            shutil.copytree(args.stage, package_root, symlinks=True)
            documentation = package_root / "share/doc/libaoahid"
            documentation.mkdir(parents=True, exist_ok=True)
            for name in ("LICENSE", "NOTICE", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md"):
                shutil.copy2(args.source_root / name, documentation / name)
            examples_source = args.source_root / "examples"
            examples_destination = documentation / "examples"
            shutil.copytree(
                examples_source,
                examples_destination,
                ignore=shutil.ignore_patterns(
                    "__pycache__", "*.pyc", "bin", "obj", "target", "*.egg-info"
                ),
            )
            (documentation / "build-metadata.json").write_text(
                json.dumps(metadata(args), indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            validate_layout(package_root, args.platform, args.variant, args.version)

            internal_sbom = package_root / generate_spdx.SPDX_NAME
            spdx_args = argparse.Namespace(
                root=package_root,
                version=args.version,
                platform=args.platform,
                arch=args.arch,
                variant=args.variant,
                source_sha=args.source_sha,
                created_epoch=args.created_epoch,
                libusb_version=args.libusb_version,
            )
            document = generate_spdx.create_document(spdx_args)
            internal_sbom.parent.mkdir(parents=True, exist_ok=True)
            internal_sbom.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            generate_spdx.validate_document(package_root, document)
            shutil.copy2(internal_sbom, sidecar)

            if args.platform == "windows":
                write_zip(package_root, archive, args.created_epoch)
            else:
                write_tar_gz(package_root, archive, args.created_epoch)
    except (
        OSError,
        ValueError,
        PackageError,
        generate_spdx.SpdxError,
        glibc_versions.GlibcVersionError,
        pkg_config_metadata.PkgConfigError,
    ) as error:
        print(f"package-release: error: {error}", file=sys.stderr)
        return 1
    print(f"package-release: wrote {archive.name} and {sidecar.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
