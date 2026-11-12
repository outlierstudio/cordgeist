#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Create and validate the deterministic source archive for a tagged commit."""

from __future__ import annotations

import argparse
from io import BytesIO
import gzip
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile


REPOSITORY_SLUG = "nemarpuc/libaoahid"
REPOSITORY_URL = f"https://github.com/{REPOSITORY_SLUG}"


class SourcePackageError(RuntimeError):
    pass


def source_archive_name(version: str) -> str:
    return f"libaoahid-{version}-source.tar.gz"


def source_download_url(version: str) -> str:
    return (
        f"{REPOSITORY_URL}/releases/download/v{version}/"
        f"{source_archive_name(version)}"
    )


def _git(repository: Path, arguments: list[str], *, binary: bool = False) -> str | bytes:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise SourcePackageError(
            f"git {' '.join(arguments)} failed with exit code {result.returncode}: {detail}"
        )
    if binary:
        return result.stdout
    try:
        return result.stdout.decode("utf-8").strip()
    except UnicodeDecodeError as error:
        raise SourcePackageError("git returned non-UTF-8 metadata") from error


def validate_identity(
    repository: Path, version: str, source_sha: str, created_epoch: int
) -> None:
    if re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version) is None:
        raise SourcePackageError("version must have the exact form MAJOR.MINOR.PATCH")
    if re.fullmatch(r"[0-9a-f]{40}", source_sha) is None:
        raise SourcePackageError("source SHA must be 40 lowercase hexadecimal characters")
    if created_epoch < 0:
        raise SourcePackageError("source epoch must not be negative")
    if not repository.is_dir():
        raise SourcePackageError(f"repository root does not exist: {repository}")
    resolved = _git(repository, ["rev-parse", "--verify", f"{source_sha}^{{commit}}"])
    if resolved != source_sha:
        raise SourcePackageError(
            f"source SHA resolves to {resolved!r}, expected exact commit {source_sha}"
        )
    commit_epoch = _git(repository, ["show", "-s", "--format=%ct", source_sha])
    if commit_epoch != str(created_epoch):
        raise SourcePackageError(
            f"source epoch {created_epoch} disagrees with commit epoch {commit_epoch}"
        )


def create_archive_bytes(
    repository: Path, version: str, source_sha: str, created_epoch: int
) -> bytes:
    validate_identity(repository, version, source_sha, created_epoch)
    prefix = f"libaoahid-{version}/"
    raw_tar = _git(
        repository,
        ["archive", "--format=tar", f"--prefix={prefix}", source_sha],
        binary=True,
    )
    assert isinstance(raw_tar, bytes)
    compressed = BytesIO()
    with gzip.GzipFile(
        filename="",
        mode="wb",
        compresslevel=9,
        fileobj=compressed,
        mtime=created_epoch,
    ) as output:
        output.write(raw_tar)
    return compressed.getvalue()


def validate_members(data: bytes, version: str, source_sha: str) -> None:
    root = f"libaoahid-{version}"
    required = {
        f"{root}/CMakeLists.txt",
        f"{root}/README.md",
        f"{root}/LICENSE",
        f"{root}/include/aoahid.h",
        f"{root}/src/api/c_api.cpp",
        f"{root}/.github/workflows/release.yml",
        f"{root}/tools/release/package_source.py",
    }
    try:
        with tarfile.open(fileobj=BytesIO(data), mode="r:gz") as archive:
            if archive.pax_headers.get("comment") != source_sha:
                raise SourcePackageError(
                    "source archive does not carry the exact git commit in its pax header"
                )
            names: set[str] = set()
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                if (
                    path.is_absolute()
                    or not path.parts
                    or path.parts[0] != root
                    or ".." in path.parts
                ):
                    raise SourcePackageError(
                        f"source archive contains an unsafe member: {member.name!r}"
                    )
                normalized = path.as_posix().rstrip("/")
                if normalized in names:
                    raise SourcePackageError(
                        f"source archive contains a duplicate member: {normalized!r}"
                    )
                names.add(normalized)
                if member.issym():
                    target = PurePosixPath(member.linkname)
                    if target.is_absolute() or ".." in target.parts:
                        raise SourcePackageError(
                            f"source archive contains an unsafe symlink: "
                            f"{member.name!r} -> {member.linkname!r}"
                        )
                elif not (member.isfile() or member.isdir()):
                    raise SourcePackageError(
                        f"source archive contains unsupported member type: {member.name!r}"
                    )
    except (gzip.BadGzipFile, tarfile.TarError) as error:
        raise SourcePackageError(f"source archive is invalid: {error}") from error
    missing = sorted(required - names)
    if missing:
        raise SourcePackageError(
            "source archive is missing required repository files: " + ", ".join(missing)
        )


def validate_source_archive(
    archive: Path,
    repository: Path,
    version: str,
    source_sha: str,
    created_epoch: int,
) -> None:
    if archive.name != source_archive_name(version):
        raise SourcePackageError(
            f"source archive is named {archive.name!r}, expected "
            f"{source_archive_name(version)!r}"
        )
    actual = archive.read_bytes()
    expected = create_archive_bytes(repository, version, source_sha, created_epoch)
    if actual != expected:
        raise SourcePackageError(
            "source archive is not the deterministic git archive of the tagged commit"
        )
    validate_members(actual, version, source_sha)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=Path, default=Path("."))
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--created-epoch", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        output = args.out_dir / source_archive_name(args.version)
        if output.exists():
            raise SourcePackageError(f"refusing to overwrite existing {output}")
        contents = create_archive_bytes(
            args.repository_root, args.version, args.source_sha, args.created_epoch
        )
        validate_members(contents, args.version, args.source_sha)
        output.write_bytes(contents)
        validate_source_archive(
            output,
            args.repository_root,
            args.version,
            args.source_sha,
            args.created_epoch,
        )
    except (OSError, ValueError, SourcePackageError) as error:
        print(f"package-source: error: {error}", file=sys.stderr)
        return 1
    print(f"package-source: wrote {output.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
