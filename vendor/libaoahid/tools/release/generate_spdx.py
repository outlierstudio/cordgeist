#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Generate and validate a deterministic SPDX 2.3 JSON document.

Normative model used here (retrieved 2026-08-27):
  * SPDX 2.3, Document Creation Information
    https://spdx.github.io/spdx-spec/v2.3/document-creation-information/
  * SPDX 2.3, Package Information (including package verification code)
    https://spdx.github.io/spdx-spec/v2.3/package-information/
  * SPDX 2.3, File Information
    https://spdx.github.io/spdx-spec/v2.3/file-information/
  * SPDX 2.3, Relationships: DESCRIBES, CONTAINS, DEPENDS_ON,
    BUILD_DEPENDENCY_OF
    https://spdx.github.io/spdx-spec/v2.3/relationships-between-SPDX-elements/
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

import package_source


SPDX_NAME = "share/doc/libaoahid/libaoahid.spdx.json"
LIBUSB_SOURCE_NAME = "share/doc/libaoahid/third-party/source/libusb-1.0.30.tar.bz2"
LIBUSB_SOURCE_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_LICENSE_NAME = "share/doc/libaoahid/third-party/libusb-copyright"
LIBUSB_LICENSE_SHA256 = "5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a"
VCPKG_COMMIT = "ddd0023b0eee70986e42ed49d9d4afb8098f212e"
VCPKG_LICENSE_NAME = "share/doc/libaoahid/third-party/vcpkg-LICENSE.txt"
VCPKG_LICENSE_SHA256 = "1ee376fc340e0aa6ad6a3581c94126e741468705096ac92263048a21daa86460"
VCPKG_PORT_FILES = (
    "share/doc/libaoahid/third-party/vcpkg-libusb-port/portfile.cmake",
    "share/doc/libaoahid/third-party/vcpkg-libusb-port/vcpkg.json",
)
VCPKG_FILE_SHA256 = {
    VCPKG_LICENSE_NAME: VCPKG_LICENSE_SHA256,
    VCPKG_PORT_FILES[0]: "63cc8c6a76d0229859e3ae280471fb625a6304db775e506677d37a3b22b736fc",
    VCPKG_PORT_FILES[1]: "865eb2f6137d52dfb3a36d7f95f76e3707811d69f9fbb45e887ccf72d0db76ed",
}


MIT_LICENSE = "MIT"
LIBUSB_LICENSE = "LGPL-2.1-or-later"
FIRST_PARTY_COPYRIGHT = "Copyright (c) 2026 libaoahid contributors"
MICROSOFT_COPYRIGHT = "Copyright (c) Microsoft Corporation"

# Files installed from the libusb dependency rather than built from this
# repository. They are the LGPL-2.1-or-later work libaoahid links dynamically.
LIBUSB_FILE_PREFIXES = (
    "include/libusb-1.0/",
    "lib/dependencies/",
)
LIBUSB_FILE_NAMES = frozenset(
    {LIBUSB_SOURCE_NAME, LIBUSB_LICENSE_NAME, "lib/pkgconfig/libusb-1.0.pc"}
)


class SpdxError(RuntimeError):
    pass


def file_license(relative: str) -> tuple[str, str]:
    """Return the (license expression, copyright text) of one packaged file.

    Package-level declarations alone leave every file NOASSERTION, which tells
    a downstream scanner nothing about which bytes are MIT and which are
    LGPL. Classification here is by installed location, which is exactly what
    the packaging step controls.
    """
    if relative in VCPKG_FILE_SHA256:
        return MIT_LICENSE, MICROSOFT_COPYRIGHT
    name = relative.rsplit("/", 1)[-1].lower()
    if (
        relative in LIBUSB_FILE_NAMES
        or relative.startswith(LIBUSB_FILE_PREFIXES)
        or ("usb" in name and not name.startswith("libaoahid") and not name.startswith("aoahid"))
    ):
        return LIBUSB_LICENSE, "NOASSERTION"
    return MIT_LICENSE, FIRST_PARTY_COPYRIGHT


def release_archive_name(
    version: str, platform: str, arch: str, variant: str
) -> str:
    allowed = {
        "linux": {"x86_64", "aarch64"},
        "macos": {"x86_64", "arm64"},
        "windows": {"x86_64", "arm64"},
    }
    if platform not in allowed or arch not in allowed[platform]:
        raise SpdxError(f"unsupported release target: {platform}/{arch}")
    if variant not in {"shared", "static"}:
        raise SpdxError(f"unsupported release variant: {variant!r}")
    if platform == "linux":
        return (
            f"libaoahid-{version}-linux-{arch}-ubuntu22.04-{variant}.tar.gz"
        )
    if platform == "macos":
        return f"libaoahid-{version}-macos-{arch}-{variant}.tar.gz"
    return f"libaoahid-{version}-windows-{arch}-{variant}.zip"


def release_download_url(
    version: str, platform: str, arch: str, variant: str
) -> str:
    archive = release_archive_name(version, platform, arch, variant)
    return (
        f"{package_source.REPOSITORY_URL}/releases/download/v{version}/{archive}"
    )


def digest(path: Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def package_files(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob("*") if path.is_file() and path.relative_to(root).as_posix() != SPDX_NAME),
        key=lambda item: item.relative_to(root).as_posix(),
    )


def verification_code(files: list[Path]) -> str:
    values = sorted(digest(path, "sha1").lower() for path in files)
    return hashlib.sha1("".join(values).encode("ascii")).hexdigest()


def file_id(relative: str) -> str:
    token = re.sub(r"[^A-Za-z0-9.-]", "-", relative)
    suffix = hashlib.sha1(relative.encode("utf-8")).hexdigest()[:12]
    return f"SPDXRef-File-{token}-{suffix}"


def create_document(args: argparse.Namespace) -> dict[str, object]:
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.libusb_version) is None:
        raise SpdxError(f"unsupported libusb version syntax: {args.libusb_version!r}")
    root = args.root.resolve()
    libusb_source = root / LIBUSB_SOURCE_NAME
    if not libusb_source.is_file() or digest(libusb_source, "sha256") != LIBUSB_SOURCE_SHA256:
        raise SpdxError("the official libusb 1.0.30 source archive is missing or has the wrong SHA-256")
    libusb_license = root / LIBUSB_LICENSE_NAME
    if not libusb_license.is_file() or digest(libusb_license, "sha256") != LIBUSB_LICENSE_SHA256:
        raise SpdxError("the exact libusb 1.0.30 license text is missing or has the wrong SHA-256")
    if args.libusb_version != "1.0.30":
        raise SpdxError("the bundled libusb source archive does not match the dependency version")
    files = package_files(root)
    if not files:
        raise SpdxError(f"package root is empty: {root}")
    created = datetime.fromtimestamp(args.created_epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    identity = f"{args.version}:{args.platform}:{args.arch}:{args.variant}:{args.source_sha}:{verification_code(files)}"
    namespace_id = uuid.uuid5(uuid.NAMESPACE_URL, identity)
    package_id = "SPDXRef-Package-libaoahid"
    libusb_id = "SPDXRef-Package-libusb"
    vcpkg_id = "SPDXRef-Package-vcpkg"
    vcpkg_paths: list[Path] = []
    if args.platform == "windows":
        vcpkg_license = root / VCPKG_LICENSE_NAME
        if not vcpkg_license.is_file() or digest(vcpkg_license, "sha256") != VCPKG_LICENSE_SHA256:
            raise SpdxError("the exact pinned vcpkg MIT license is missing or has the wrong SHA-256")
        vcpkg_paths = [vcpkg_license]
        for relative in VCPKG_PORT_FILES:
            path = root / relative
            if (
                not path.is_file()
                or digest(path, "sha256") != VCPKG_FILE_SHA256[relative]
            ):
                raise SpdxError(
                    f"pinned vcpkg build material is missing or changed: {relative}"
                )
            vcpkg_paths.append(path)

    spdx_files: list[dict[str, object]] = []
    relationships: list[dict[str, str]] = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": package_id,
        },
        {
            "spdxElementId": package_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": libusb_id,
        },
    ]
    if vcpkg_paths:
        relationships.append(
            {
                "spdxElementId": vcpkg_id,
                "relationshipType": "BUILD_DEPENDENCY_OF",
                "relatedSpdxElement": package_id,
            }
        )
    observed_licenses: set[str] = set()
    for path in files:
        relative = path.relative_to(root).as_posix()
        identifier = file_id(relative)
        license_expression, copyright_text = file_license(relative)
        observed_licenses.add(license_expression)
        spdx_files.append(
            {
                "fileName": "./" + relative,
                "SPDXID": identifier,
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": digest(path, "sha1")},
                    {"algorithm": "SHA256", "checksumValue": digest(path, "sha256")},
                ],
                "licenseConcluded": license_expression,
                "licenseInfoInFiles": [license_expression],
                "copyrightText": copyright_text,
            }
        )
        relationships.append(
            {
                "spdxElementId": package_id,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": identifier,
            }
        )
        if path in vcpkg_paths:
            relationships.append(
                {
                    "spdxElementId": vcpkg_id,
                    "relationshipType": "CONTAINS",
                    "relatedSpdxElement": identifier,
                }
            )

    packages: list[dict[str, object]] = [
        {
            "name": "libaoahid",
            "SPDXID": package_id,
            "versionInfo": args.version,
            "downloadLocation": release_download_url(
                args.version, args.platform, args.arch, args.variant
            ),
            "packageFileName": release_archive_name(
                args.version, args.platform, args.arch, args.variant
            ),
            "homepage": package_source.REPOSITORY_URL,
            "filesAnalyzed": True,
            "packageVerificationCode": {
                "packageVerificationCodeValue": verification_code(files),
                "packageVerificationCodeExcludedFiles": [SPDX_NAME],
            },
            "licenseConcluded": " AND ".join(sorted(observed_licenses)),
            "licenseDeclared": MIT_LICENSE,
            "licenseInfoFromFiles": sorted(observed_licenses),
            "copyrightText": FIRST_PARTY_COPYRIGHT,
            "supplier": "NOASSERTION",
            "primaryPackagePurpose": "LIBRARY",
            "sourceInfo": f"Git commit {args.source_sha}",
        },
        {
            "name": "libusb",
            "SPDXID": libusb_id,
            "versionInfo": args.libusb_version,
            "downloadLocation": (
                "https://github.com/libusb/libusb/releases/download/v1.0.30/"
                "libusb-1.0.30.tar.bz2"
            ),
            "packageFileName": "libusb-1.0.30.tar.bz2",
            "checksums": [
                {"algorithm": "SHA256", "checksumValue": LIBUSB_SOURCE_SHA256}
            ],
            "filesAnalyzed": False,
            "licenseConcluded": LIBUSB_LICENSE,
            "licenseDeclared": LIBUSB_LICENSE,
            "copyrightText": "NOASSERTION",
            "supplier": "NOASSERTION",
            "primaryPackagePurpose": "LIBRARY",
        },
    ]
    if vcpkg_paths:
        packages.append(
            {
                "name": "vcpkg",
                "SPDXID": vcpkg_id,
                "versionInfo": VCPKG_COMMIT,
                "downloadLocation": f"https://github.com/microsoft/vcpkg/tree/{VCPKG_COMMIT}",
                "filesAnalyzed": True,
                "packageVerificationCode": {
                    "packageVerificationCodeValue": verification_code(vcpkg_paths)
                },
                "licenseConcluded": MIT_LICENSE,
                "licenseDeclared": MIT_LICENSE,
                "licenseInfoFromFiles": [MIT_LICENSE],
                "copyrightText": MICROSOFT_COPYRIGHT,
                "supplier": "Organization: Microsoft Corporation",
                "primaryPackagePurpose": "SOURCE",
                "sourceInfo": "Pinned vcpkg libusb port build material",
            }
        )

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"libaoahid-{args.version}-{args.platform}-{args.arch}-{args.variant}",
        "documentNamespace": f"https://spdx.org/spdxdocs/libaoahid-{namespace_id}",
        "creationInfo": {"created": created, "creators": ["Tool: libaoahid-generate-spdx"]},
        "documentDescribes": [package_id],
        "packages": packages,
        "files": spdx_files,
        "relationships": relationships,
    }


def validate_document(root: Path, document: dict[str, object]) -> None:
    if document.get("spdxVersion") != "SPDX-2.3" or document.get("dataLicense") != "CC0-1.0":
        raise SpdxError("document is not SPDX-2.3 JSON")
    packages = document.get("packages")
    if not isinstance(packages, list):
        raise SpdxError("packages must be a list")
    own = next((p for p in packages if isinstance(p, dict) and p.get("SPDXID") == "SPDXRef-Package-libaoahid"), None)
    if own is None or own.get("filesAnalyzed") is not True:
        raise SpdxError("libaoahid package with filesAnalyzed=true is missing")
    name = document.get("name")
    if not isinstance(name, str):
        raise SpdxError("document name is missing")
    match = re.fullmatch(
        r"libaoahid-([0-9]+\.[0-9]+\.[0-9]+)-(linux|macos|windows)-"
        r"(x86_64|aarch64|arm64)-(shared|static)",
        name,
    )
    if match is None:
        raise SpdxError("document name does not identify a supported release target")
    version, platform, arch, variant = match.groups()
    if own.get("versionInfo") != version:
        raise SpdxError("libaoahid package version disagrees with the SPDX document name")
    if own.get("homepage") != package_source.REPOSITORY_URL:
        raise SpdxError("libaoahid SPDX homepage is not the canonical repository")
    expected_download = release_download_url(version, platform, arch, variant)
    if own.get("downloadLocation") != expected_download:
        raise SpdxError("libaoahid SPDX download location is not the exact release asset")
    if own.get("packageFileName") != release_archive_name(
        version, platform, arch, variant
    ):
        raise SpdxError("libaoahid SPDX package file name is not the release archive")
    dependency = next(
        (p for p in packages if isinstance(p, dict) and p.get("SPDXID") == "SPDXRef-Package-libusb"),
        None,
    )
    if dependency is None or dependency.get("packageFileName") != "libusb-1.0.30.tar.bz2":
        raise SpdxError("libusb source-package record is missing")
    dependency_checksums = {
        item.get("algorithm"): item.get("checksumValue")
        for item in dependency.get("checksums", [])
        if isinstance(item, dict)
    }
    if dependency_checksums.get("SHA256") != LIBUSB_SOURCE_SHA256:
        raise SpdxError("libusb source-package checksum is incorrect")
    libusb_license = root.resolve() / LIBUSB_LICENSE_NAME
    if not libusb_license.is_file() or digest(libusb_license, "sha256") != LIBUSB_LICENSE_SHA256:
        raise SpdxError("the exact libusb 1.0.30 license text is missing or has the wrong SHA-256")

    vcpkg_license = root.resolve() / VCPKG_LICENSE_NAME
    expected_vcpkg_paths = [vcpkg_license] + [
        root.resolve() / relative for relative in VCPKG_PORT_FILES
    ]
    present_vcpkg_paths = [path for path in expected_vcpkg_paths if path.is_file()]
    if present_vcpkg_paths and len(present_vcpkg_paths) != len(expected_vcpkg_paths):
        raise SpdxError("bundled vcpkg material is incomplete")
    for path in present_vcpkg_paths:
        relative = path.relative_to(root.resolve()).as_posix()
        if digest(path, "sha256") != VCPKG_FILE_SHA256[relative]:
            raise SpdxError(f"bundled vcpkg material changed: {relative}")
    vcpkg = next(
        (p for p in packages if isinstance(p, dict) and p.get("SPDXID") == "SPDXRef-Package-vcpkg"),
        None,
    )
    if vcpkg_license.is_file():
        if len(packages) != 3:
            raise SpdxError("a Windows SPDX document must contain exactly three packages")
        if digest(vcpkg_license, "sha256") != VCPKG_LICENSE_SHA256:
            raise SpdxError("the pinned vcpkg license has the wrong SHA-256")
        vcpkg_paths = expected_vcpkg_paths
        if (
            not isinstance(vcpkg, dict)
            or vcpkg.get("versionInfo") != VCPKG_COMMIT
            or vcpkg.get("licenseConcluded") != "MIT"
            or vcpkg.get("licenseDeclared") != "MIT"
            or vcpkg.get("filesAnalyzed") is not True
        ):
            raise SpdxError("the pinned vcpkg SPDX package record is incomplete")
        vcpkg_code = vcpkg.get("packageVerificationCode")
        if (
            not isinstance(vcpkg_code, dict)
            or vcpkg_code.get("packageVerificationCodeValue")
            != verification_code(vcpkg_paths)
        ):
            raise SpdxError("the pinned vcpkg SPDX verification code is incorrect")
    elif vcpkg is not None:
        raise SpdxError("a vcpkg SPDX package record exists without bundled vcpkg material")
    elif len(packages) != 2:
        raise SpdxError("a non-Windows SPDX document must contain exactly two packages")

    files = package_files(root.resolve())
    expected = {"./" + p.relative_to(root.resolve()).as_posix(): p for p in files}
    listed = document.get("files")
    if not isinstance(listed, list):
        raise SpdxError("files must be a list")
    actual_names = [item.get("fileName") for item in listed if isinstance(item, dict)]
    if len(actual_names) != len(listed) or len(actual_names) != len(set(actual_names)):
        raise SpdxError("SPDX file list contains a malformed or duplicate record")
    if set(actual_names) != set(expected):
        raise SpdxError("SPDX file list does not match package contents")
    expected_file_ids: set[str] = set()
    observed_licenses: set[str] = set()
    for item in listed:
        assert isinstance(item, dict)
        path = expected[str(item["fileName"])]
        relative = path.relative_to(root.resolve()).as_posix()
        expected_identifier = file_id(relative)
        if item.get("SPDXID") != expected_identifier:
            raise SpdxError(f"unexpected SPDX file identifier: {item['fileName']}")
        expected_file_ids.add(expected_identifier)
        license_expression, copyright_text = file_license(relative)
        observed_licenses.add(license_expression)
        if (
            item.get("licenseConcluded") != license_expression
            or item.get("licenseInfoInFiles") != [license_expression]
            or item.get("copyrightText") != copyright_text
        ):
            raise SpdxError(f"file-level license is inconsistent: {item['fileName']}")
        checksums = {
            entry.get("algorithm"): entry.get("checksumValue")
            for entry in item.get("checksums", [])
            if isinstance(entry, dict)
        }
        if checksums.get("SHA1") != digest(path, "sha1") or checksums.get("SHA256") != digest(path, "sha256"):
            raise SpdxError(f"checksum mismatch: {item['fileName']}")
    code = own.get("packageVerificationCode")
    if not isinstance(code, dict) or code.get("packageVerificationCodeValue") != verification_code(files):
        raise SpdxError("package verification code mismatch")
    if (
        own.get("licenseDeclared") != MIT_LICENSE
        or own.get("licenseConcluded") != " AND ".join(sorted(observed_licenses))
        or own.get("licenseInfoFromFiles") != sorted(observed_licenses)
    ):
        raise SpdxError("package license summary disagrees with the file-level licenses")
    if LIBUSB_LICENSE not in observed_licenses:
        raise SpdxError("no packaged file is attributed to the bundled libusb runtime")
    relationships = document.get("relationships")
    if not isinstance(relationships, list):
        raise SpdxError("relationships must be a list")
    if document.get("documentDescribes") != ["SPDXRef-Package-libaoahid"]:
        raise SpdxError("documentDescribes does not identify exactly libaoahid")

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
    if vcpkg_license.is_file():
        expected_relationships.add(
            (
                "SPDXRef-Package-vcpkg",
                "BUILD_DEPENDENCY_OF",
                "SPDXRef-Package-libaoahid",
            )
        )
        expected_relationships.update(
            (
                "SPDXRef-Package-vcpkg",
                "CONTAINS",
                file_id(path.relative_to(root.resolve()).as_posix()),
            )
            for path in vcpkg_paths
        )

    actual_relationships: list[tuple[str, str, str]] = []
    for item in relationships:
        if not isinstance(item, dict):
            raise SpdxError("relationship record is not an object")
        element = item.get("spdxElementId")
        relation = item.get("relationshipType")
        related = item.get("relatedSpdxElement")
        if not all(isinstance(value, str) for value in (element, relation, related)):
            raise SpdxError("relationship record is incomplete")
        actual_relationships.append((element, relation, related))
    if (
        len(actual_relationships) != len(set(actual_relationships))
        or set(actual_relationships) != expected_relationships
    ):
        raise SpdxError("SPDX relationship graph does not exactly match package contents")

    if vcpkg_license.is_file():
        expected_vcpkg_file_ids = {
            file_id(path.relative_to(root.resolve()).as_posix())
            for path in vcpkg_paths
        }
        actual_vcpkg_file_ids = {
            str(item.get("relatedSpdxElement"))
            for item in relationships
            if isinstance(item, dict)
            and item.get("spdxElementId") == "SPDXRef-Package-vcpkg"
            and item.get("relationshipType") == "CONTAINS"
        }
        if actual_vcpkg_file_ids != expected_vcpkg_file_ids:
            raise SpdxError("the vcpkg SPDX file relationships are incomplete")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subparsers = result.add_subparsers(dest="command", required=True)
    generate = subparsers.add_parser("generate")
    generate.add_argument("--root", type=Path, required=True)
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument("--version", required=True)
    generate.add_argument("--platform", required=True)
    generate.add_argument("--arch", required=True)
    generate.add_argument("--variant", choices=("shared", "static"), required=True)
    generate.add_argument("--source-sha", required=True)
    generate.add_argument("--created-epoch", type=int, required=True)
    generate.add_argument("--libusb-version", required=True)
    validate = subparsers.add_parser("validate")
    validate.add_argument("--root", type=Path, required=True)
    validate.add_argument("--input", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "generate":
            document = create_document(args)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            validate_document(args.root, document)
        else:
            document = json.loads(args.input.read_text(encoding="utf-8"))
            validate_document(args.root, document)
    except (OSError, ValueError, json.JSONDecodeError, SpdxError) as error:
        print(f"spdx: error: {error}", file=sys.stderr)
        return 1
    print(f"spdx: {args.command} succeeded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
