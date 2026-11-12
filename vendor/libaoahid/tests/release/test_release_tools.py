#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Focused fail-closed tests for release metadata inspectors."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
from io import BytesIO
from pathlib import Path
import os
import re
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools" / "release"))
sys.path.insert(0, str(REPOSITORY / "tools" / "symbol-lint"))
sys.path.insert(0, str(REPOSITORY / "tools"))

import check_license_headers  # noqa: E402
import glibc_versions  # noqa: E402
import collect_release  # noqa: E402
import generate_spdx  # noqa: E402
import package_release  # noqa: E402
import package_source  # noqa: E402
import pkg_config_metadata  # noqa: E402
import sync_version  # noqa: E402
import validate_release  # noqa: E402
import version_files  # noqa: E402
import symbol_lint  # noqa: E402


class SymbolLintTests(unittest.TestCase):
    def test_macos_parser_keeps_only_true_dynamic_exports(self) -> None:
        fixture = """\
0000000000001000 (__TEXT,__text) external _aoahid_version
0000000000001010 (__TEXT,__text) weak external _aoahid_weak
0000000000001020 (__TEXT,__text) private external ___dso_handle
0000000000001030 (__TEXT,__text) weak private external _private_weak
0000000000001040 (__TEXT,__text) weak external automatically hidden _hidden
0000000000001050 (__TEXT,__text) non-external _local
0000000000001060 (__TEXT,__text) external [Thumb] _unexpected
"""
        self.assertEqual(
            symbol_lint.parse_macos_nm_exports(fixture),
            {"aoahid_version", "aoahid_weak", "_unexpected"},
        )


class GlibcVersionTests(unittest.TestCase):
    def fake_readelf(self, directory: Path, output: str) -> Path:
        script = directory / "fake-readelf"
        script.write_text(
            "#!/usr/bin/env python3\n"
            f"print({output!r})\n",
            encoding="utf-8",
        )
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        return script

    def test_numeric_requirements_are_canonicalized(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / "libfixture.so"
            library.touch()
            readelf = self.fake_readelf(
                root,
                "Name: GLIBC_2.17 Flags: none\\n"
                "Name: GLIBC_2.2.5 Flags: none\\n"
                "Name: GLIBC_2.17 Flags: none",
            )
            self.assertEqual(
                glibc_versions.inspect(str(readelf), library),
                ["GLIBC_2.2.5", "GLIBC_2.17"],
            )

    def test_nonnumeric_requirement_is_rejected(self) -> None:
        for unsupported in ("GLIBC_PRIVATE", "GLIBC_ABI_DT_RELR"):
            with self.subTest(unsupported=unsupported), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                library = root / "libfixture.so"
                library.touch()
                readelf = self.fake_readelf(
                    root,
                    f"Name: GLIBC_2.17 Flags: none\\nName: {unsupported} Flags: none",
                )
                with self.assertRaisesRegex(
                    glibc_versions.GlibcVersionError,
                    "unsupported nonnumeric GLIBC requirements",
                ):
                    glibc_versions.inspect(str(readelf), library)


class AbiNamespacePolicyTests(unittest.TestCase):
    def test_major_one_minor_bumps_keep_the_1_0_namespace(self) -> None:
        for version in ("1.0.0", "1.1.0", "1.99.7"):
            with self.subTest(version=version):
                validate_release.validate_abi_namespace(version, "1", "1.0")

    def test_project_minor_is_not_an_abi_namespace(self) -> None:
        with self.assertRaisesRegex(
            validate_release.ValidationError,
            r"stable namespace 1\.0",
        ):
            validate_release.validate_abi_namespace("1.1.0", "1", "1.1")

    def test_soversion_must_match_project_major(self) -> None:
        with self.assertRaisesRegex(
            validate_release.ValidationError,
            "SOVERSION 1 disagrees with project major 2",
        ):
            validate_release.validate_abi_namespace("2.0.0", "1", "1.0")

    def test_new_abi_major_requires_an_explicit_namespace_policy(self) -> None:
        # "99" stands in for a next major with no policy entry yet.
        with self.assertRaisesRegex(
            validate_release.ValidationError,
            "no ELF ABI namespace policy is defined",
        ):
            validate_release.validate_abi_namespace("99.0.0", "99", "99.0")


class VersionSyncTests(unittest.TestCase):
    """sync_version.py / version_files.py: the single-command version bump.

    These copy only the files version_files.SITES names, into a scratch
    directory, so the round trip can rewrite them without touching the real
    tree the rest of this test run still depends on.
    """

    def _copy_tracked_files(self, root: Path) -> None:
        for relative in sorted({site.path for site in version_files.SITES}):
            source = REPOSITORY / relative
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())

    def test_write_all_rewrites_every_site_to_the_same_new_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            before = version_files.assembled_versions(root)
            self.assertEqual(len(set(before.values())), 1, before)

            changed = version_files.write_all(root, "9.9.9")

            self.assertEqual(set(changed), {site.path for site in version_files.SITES})
            after = version_files.assembled_versions(root)
            self.assertTrue(all(value == "9.9.9" for value in after.values()), after)

    def test_write_all_is_a_no_op_when_the_version_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            current = next(iter(version_files.assembled_versions(root).values()))

            changed = version_files.write_all(root, current)

            self.assertEqual(changed, [])

    def test_write_all_rejects_a_malformed_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            with self.assertRaises(version_files.VersionFileError):
                version_files.write_all(root, "not-a-version")

    def test_assembled_versions_reports_disagreement_within_one_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            current = next(iter(version_files.assembled_versions(root).values()))
            mismatched = "9.9.8" if current != "9.9.8" else "9.9.9"
            rust_manifest = root / "bindings/rust/aoahid/Cargo.toml"
            text = rust_manifest.read_text(encoding="utf-8")
            rust_manifest.write_text(
                text.replace(f'version = "{current}"', f'version = "{mismatched}"', 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(version_files.VersionFileError, "disagree"):
                version_files.assembled_versions(root)

    def test_sync_version_cli_writes_files_and_reports_success(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            with mock.patch("sys.argv", ["sync_version.py", "1.2.3", "--root", str(root), "--no-generate"]):
                exit_code = sync_version.main()
            self.assertEqual(exit_code, 0)
            after = version_files.assembled_versions(root)
            self.assertTrue(all(value == "1.2.3" for value in after.values()), after)

    def test_sync_version_cli_rejects_a_malformed_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._copy_tracked_files(root)
            with mock.patch("sys.argv", ["sync_version.py", "not-a-version", "--root", str(root)]):
                exit_code = sync_version.main()
            self.assertEqual(exit_code, 1)


class SourcePackageTests(unittest.TestCase):
    def git(self, repository: Path, *arguments: str, env: dict[str, str] | None = None) -> str:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        )
        return result.stdout.strip()

    def fixture_repository(self, root: Path) -> tuple[str, int]:
        files = {
            ".gitignore": (REPOSITORY / ".gitignore").read_text(encoding="utf-8"),
            "CMakeLists.txt": "project(libaoahid VERSION 0.1.0)\n",
            "README.md": "# libaoahid\n",
            "LICENSE": "fixture license\n",
            "include/aoahid.h": "#pragma once\n",
            "src/api/c_api.cpp": "int fixture;\n",
            ".github/workflows/release.yml": "name: Release\n",
            "tools/release/package_source.py": "# fixture\n",
        }
        for relative, contents in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        generated = {
            "build-manual/aoahid.dll": b"generated DLL",
            "build-local/libaoahid.so": b"generated shared object",
            "dist/libaoahid-source.zip": b"generated archive",
            "out/release-manifest.json": b"generated manifest",
            ".vcpkg/installed/status": b"generated cache",
            ".pytest_cache/README.md": b"generated cache",
            "bindings/python/.pytest_cache/README.md": b"nested generated cache",
            "tools/release/__pycache__/module.pyc": b"nested bytecode cache",
        }
        for relative, contents in generated.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        self.git(root, "init", "-b", "main")
        self.git(root, "config", "user.name", "Release Test")
        self.git(root, "config", "user.email", "release-test@example.invalid")
        self.git(root, "add", "-A")
        tracked = set(self.git(root, "ls-files").splitlines())
        self.assertTrue(set(files).issubset(tracked))
        self.assertTrue(set(generated).isdisjoint(tracked))
        environment = os.environ.copy()
        environment["GIT_AUTHOR_DATE"] = "2026-08-27T00:00:00Z"
        environment["GIT_COMMITTER_DATE"] = environment["GIT_AUTHOR_DATE"]
        self.git(root, "commit", "-m", "fixture", env=environment)
        source_sha = self.git(root, "rev-parse", "HEAD")
        epoch = int(self.git(root, "show", "-s", "--format=%ct", source_sha))
        return source_sha, epoch

    def test_source_archive_is_exact_and_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_sha, epoch = self.fixture_repository(root)
            first = package_source.create_archive_bytes(root, "0.1.0", source_sha, epoch)
            second = package_source.create_archive_bytes(root, "0.1.0", source_sha, epoch)
            self.assertEqual(first, second)
            package_source.validate_members(first, "0.1.0", source_sha)
            with tarfile.open(fileobj=BytesIO(first), mode="r:gz") as source_tar:
                archived = {member.name for member in source_tar.getmembers()}
            for generated in (
                "build-manual/",
                "build-local/",
                "dist/",
                "out/",
                ".vcpkg/",
                ".pytest_cache/",
                "bindings/python/.pytest_cache/",
                "tools/release/__pycache__/",
            ):
                self.assertFalse(
                    any(f"libaoahid-0.1.0/{generated}" in name for name in archived),
                    generated,
                )
            archive = root / package_source.source_archive_name("0.1.0")
            archive.write_bytes(first)
            package_source.validate_source_archive(
                archive, root, "0.1.0", source_sha, epoch
            )
            archive.write_bytes(first + b"tampered")
            with self.assertRaisesRegex(
                package_source.SourcePackageError, "deterministic git archive"
            ):
                package_source.validate_source_archive(
                    archive, root, "0.1.0", source_sha, epoch
                )


class ReleaseIdentityTests(unittest.TestCase):
    def test_exact_release_archive_names_and_urls(self) -> None:
        cases = {}
        for platform, arch, qualifier, extension in collect_release.TARGETS:
            platform_part = f"{platform}-{arch}" + (
                f"-{qualifier}" if qualifier else ""
            )
            for variant in collect_release.VARIANTS:
                cases[(platform, arch, variant)] = (
                    f"libaoahid-0.1.0-{platform_part}-{variant}{extension}"
                )
        for (platform, arch, variant), expected in cases.items():
            with self.subTest(platform=platform, arch=arch, variant=variant):
                self.assertEqual(
                    generate_spdx.release_archive_name(
                        "0.1.0", platform, arch, variant
                    ),
                    expected,
                )
                self.assertEqual(
                    generate_spdx.release_download_url(
                        "0.1.0", platform, arch, variant
                    ),
                    f"{package_source.REPOSITORY_URL}/releases/download/v0.1.0/{expected}",
                )
        with self.assertRaises(generate_spdx.SpdxError):
            generate_spdx.release_archive_name(
                "0.1.0", "linux", "arm64", "shared"
            )
        self.assertEqual(
            package_source.source_download_url("0.1.0"),
            f"{package_source.REPOSITORY_URL}/releases/download/v0.1.0/"
            "libaoahid-0.1.0-source.tar.gz",
        )

    def test_exact_33_asset_contract(self) -> None:
        assets = collect_release.expected_release_asset_names("0.1.0")
        self.assertEqual(len(assets), 33)
        self.assertEqual(
            sum(name.endswith((".tar.gz", ".zip")) for name in assets),
            19,
        )
        self.assertEqual(sum(name.endswith(".spdx.json") for name in assets), 12)
        self.assertEqual(
            sum(name.endswith(("-runtime.tar.gz", "-runtime.zip")) for name in assets),
            6,
        )
        # A bare shared library cannot carry libusb's license or corresponding
        # source, so no asset may be one.
        self.assertEqual(sum(name.endswith((".so", ".dylib", ".dll")) for name in assets), 0)
        self.assertIn("release-manifest.json", assets)
        self.assertIn("SHA256SUMS", assets)
        readme = (REPOSITORY / "README.md").read_text(encoding="utf-8")
        self.assertIn("exactly 33 uploaded assets", readme)

    def test_runtime_bundle_carries_every_license_obligation(self) -> None:
        elf = bytearray(64)
        elf[0:4] = b"\x7fELF"
        elf[4] = 2
        elf[5] = 1
        struct.pack_into("<HH", elf, 16, 3, 62)
        root = "libaoahid-0.1.0-linux-x86_64-ubuntu22.04-shared"
        libusb_source = b"official libusb 1.0.30 source archive"
        libusb_license = b"GNU LESSER GENERAL PUBLIC LICENSE Version 2.1"
        files = {
            "lib/libaoahid.so.0.1.0": bytes(elf),
            "lib/libusb-1.0.so.0.6.0": b"libusb runtime",
            "include/aoahid.h": b"header that must not be bundled",
            "share/doc/libaoahid/LICENSE": b"MIT",
            "share/doc/libaoahid/NOTICE": b"NOTICE",
            "share/doc/libaoahid/THIRD_PARTY_NOTICES.md": b"third party",
            "share/doc/libaoahid/build-metadata.json": b"{}",
            collect_release.LIBUSB_LICENSE_NAME: libusb_license,
            collect_release.LIBUSB_SOURCE_NAME: libusb_source,
        }
        links = {
            "lib/libaoahid.so.0": "libaoahid.so.0.1.0",
            "lib/libusb-1.0.so.0": "libusb-1.0.so.0.6.0",
        }
        with tempfile.TemporaryDirectory() as directory:
            root_path = Path(directory)
            archive = root_path / (root + ".tar.gz")
            with archive.open("wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
                    with tarfile.open(
                        mode="w", fileobj=gz, format=tarfile.PAX_FORMAT
                    ) as output:
                        for name, payload in files.items():
                            info = tarfile.TarInfo(f"{root}/{name}")
                            info.size = len(payload)
                            info.mode = 0o644
                            output.addfile(info, io.BytesIO(payload))
                        for name, target in links.items():
                            info = tarfile.TarInfo(f"{root}/{name}")
                            info.type = tarfile.SYMTYPE
                            info.linkname = target
                            output.addfile(info)
            with mock.patch.object(
                collect_release,
                "LIBUSB_SOURCE_SHA256",
                hashlib.sha256(libusb_source).hexdigest(),
            ), mock.patch.object(
                collect_release,
                "LIBUSB_LICENSE_SHA256",
                hashlib.sha256(libusb_license).hexdigest(),
            ):
                bundle = collect_release.build_runtime_bundle(
                    root_path, archive, "0.1.0", "linux", "x86_64", "ubuntu22.04", 1700000000
                )
                collect_release.validate_runtime_bundle(
                    bundle, archive, "linux", "x86_64"
                )
                self.assertEqual(
                    bundle.name, "libaoahid-0.1.0-linux-x86_64-ubuntu22.04-runtime.tar.gz"
                )
                members = collect_release.archive_members(bundle)
                for obligation in (
                    collect_release.LIBUSB_LICENSE_NAME,
                    collect_release.LIBUSB_SOURCE_NAME,
                    "share/doc/libaoahid/LICENSE",
                    "share/doc/libaoahid/NOTICE",
                    "share/doc/libaoahid/THIRD_PARTY_NOTICES.md",
                ):
                    self.assertIn(obligation, members)
                # The runtime bundle is not a development package.
                self.assertNotIn("include/aoahid.h", members)
                self.assertEqual(
                    collect_release.archive_symlinks(bundle),
                    {
                        "lib/libaoahid.so.0": "libaoahid.so.0.1.0",
                        "lib/libusb-1.0.so.0": "libusb-1.0.so.0.6.0",
                    },
                )
                first = hashlib.sha256(bundle.read_bytes()).hexdigest()
                bundle.unlink()
                again = collect_release.build_runtime_bundle(
                    root_path, archive, "0.1.0", "linux", "x86_64", "ubuntu22.04", 1700000000
                )
                self.assertEqual(
                    first, hashlib.sha256(again.read_bytes()).hexdigest()
                )

    def test_macos_runtime_is_checked_as_a_thin_mach_o_dylib(self) -> None:
        def dylib(cpu_type: int, file_type: int = 6) -> bytes:
            header = bytearray(32)
            header[0:4] = b"\xcf\xfa\xed\xfe"
            struct.pack_into("<iiI", header, 4, cpu_type, 0, file_type)
            return bytes(header)

        collect_release.validate_runtime_architecture(
            dylib(0x0100000C), "macos", "arm64"
        )
        collect_release.validate_runtime_architecture(
            dylib(0x01000007), "macos", "x86_64"
        )
        for data, arch in (
            (dylib(0x01000007), "arm64"),
            (dylib(0x0100000C), "x86_64"),
            (dylib(0x0100000C, 2), "arm64"),
            (b"\xca\xfe\xba\xbe" + bytes(28), "arm64"),
        ):
            with self.subTest(arch=arch, magic=data[:4]):
                with self.assertRaises(collect_release.CollectionError):
                    collect_release.validate_runtime_architecture(
                        data, "macos", arch
                    )
        self.assertTrue(collect_release.is_runtime_binary("lib/libusb-1.0.0.dylib", "macos"))
        self.assertFalse(collect_release.is_runtime_binary("lib/libaoahid.a", "macos"))
        self.assertEqual(
            collect_release.runtime_bundle_name("0.1.0", "macos", "arm64", ""),
            "libaoahid-0.1.0-macos-arm64-runtime.tar.gz",
        )

    def test_spdx_files_are_licensed_individually(self) -> None:
        self.assertEqual(
            generate_spdx.file_license("lib/libaoahid.so.0.1.0"),
            (generate_spdx.MIT_LICENSE, generate_spdx.FIRST_PARTY_COPYRIGHT),
        )
        for dependency in (
            "lib/libusb-1.0.so.0.6.0",
            "lib/libusb-1.0.0.dylib",
            "bin/libusb-1.0.dll",
            "include/libusb-1.0/libusb.h",
            "lib/pkgconfig/libusb-1.0.pc",
            "lib/dependencies/libusb-1.0.lib",
            collect_release.LIBUSB_LICENSE_NAME,
            collect_release.LIBUSB_SOURCE_NAME,
        ):
            self.assertEqual(
                generate_spdx.file_license(dependency)[0],
                generate_spdx.LIBUSB_LICENSE,
                dependency,
            )
        self.assertEqual(
            generate_spdx.file_license(collect_release.VCPKG_LICENSE_NAME),
            (generate_spdx.MIT_LICENSE, generate_spdx.MICROSOFT_COPYRIGHT),
        )

    def test_license_header_uses_each_language_comment_syntax(self) -> None:
        # A Kotlin build script rejects a '#' line outright, so the header
        # prefix has to follow the language's own comment syntax rather than
        # the file's role. Assert the mapping explicitly, then confirm no file
        # actually carries the wrong one.
        expected_prefix = {
            ".c": "// ",
            ".h": "// ",
            ".cpp": "// ",
            ".hpp": "// ",
            ".rs": "// ",
            ".cs": "// ",
            ".java": "// ",
            ".kt": "// ",
            ".kts": "// ",
            ".py": "# ",
            ".cmake": "# ",
            ".sh": "# ",
        }
        for suffix, prefix in expected_prefix.items():
            self.assertEqual(
                check_license_headers.header_prefix(Path(f"fixture{suffix}")),
                prefix,
                suffix,
            )
        self.assertEqual(
            check_license_headers.header_prefix(Path("CMakeLists.txt")), "# "
        )
        for path in check_license_headers.sources():
            # Only the header region matters; a file may legitimately mention
            # the other syntax in its body, as this test file does.
            header = "\n".join(path.read_text(encoding="utf-8").splitlines()[:3])
            wrong = (
                "# SPDX-License-Identifier"
                if check_license_headers.header_prefix(path) == "// "
                else "// SPDX-License-Identifier"
            )
            self.assertNotIn(wrong, header, path)

    def test_every_source_file_carries_the_spdx_header(self) -> None:
        files = check_license_headers.sources()
        self.assertGreater(len(files), 50)
        failures = [
            message
            for message in (check_license_headers.check(path) for path in files)
            if message
        ]
        self.assertEqual(failures, [])

    def test_pkg_config_requires_the_canonical_repository_url(self) -> None:
        fixture = b"""\
prefix=${pcfiledir}/../..
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include
Name: libaoahid
Description: fixture
Version: 0.1.0
URL: https://github.com/nemarpuc/libaoahid
Libs: -L${libdir} -laoahid
Cflags: -I${includedir}
"""
        pkg_config_metadata.validate_relocatable(
            fixture,
            label="fixture",
            expected_name="libaoahid",
            expected_version="0.1.0",
            expected_url=package_source.REPOSITORY_URL,
            expected_library="aoahid",
        )
        with self.assertRaisesRegex(pkg_config_metadata.PkgConfigError, "URL"):
            pkg_config_metadata.validate_relocatable(
                fixture.replace(b"nemarpuc/libaoahid", b"wrong/repository"),
                label="fixture",
                expected_name="libaoahid",
                expected_version="0.1.0",
                expected_url=package_source.REPOSITORY_URL,
                expected_library="aoahid",
            )

    def test_build_metadata_has_exact_repository_and_asset_url(self) -> None:
        arguments = argparse.Namespace(
            version="0.1.0",
            platform="windows",
            arch="arm64",
            variant="shared",
            source_sha="a" * 40,
            created_epoch=0,
            libusb_version="1.0.30",
            build_metadata=[],
        )
        metadata = package_release.metadata(arguments)
        self.assertEqual(metadata["repository"], package_source.REPOSITORY_URL)
        self.assertEqual(
            metadata["release_download_url"],
            f"{package_source.REPOSITORY_URL}/releases/download/v0.1.0/"
            "libaoahid-0.1.0-windows-arm64-shared.zip",
        )

    def test_spdx_rejects_noncanonical_package_urls(self) -> None:
        own = {
            "SPDXID": "SPDXRef-Package-libaoahid",
            "versionInfo": "0.1.0",
            "filesAnalyzed": True,
            "homepage": "https://github.com/wrong/repository",
            "downloadLocation": "https://example.invalid/archive.zip",
            "packageFileName": "libaoahid-0.1.0-windows-arm64-shared.zip",
        }
        document = {
            "spdxVersion": "SPDX-2.3",
            "dataLicense": "CC0-1.0",
            "name": "libaoahid-0.1.0-windows-arm64-shared",
            "packages": [own],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(generate_spdx.SpdxError, "homepage"):
                generate_spdx.validate_document(root, document)
            own["homepage"] = package_source.REPOSITORY_URL
            with self.assertRaisesRegex(generate_spdx.SpdxError, "download location"):
                generate_spdx.validate_document(root, document)

    def test_release_workflow_has_all_native_targets_and_source(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("AOAHID_EXPECTED_REPOSITORY: nemarpuc/libaoahid", workflow)
        for label in (
            "Linux x86-64 (Ubuntu 22.04 glibc)",
            "Linux AArch64 (Ubuntu 22.04 glibc)",
            "Windows x64",
            "Windows ARM64",
            "macOS ARM64",
            "macOS x86-64",
        ):
            self.assertIn(label, workflow)
        self.assertIn("os: macos-15\n", workflow)
        self.assertIn("os: macos-15-intel", workflow)
        self.assertIn("os: windows-2025-vs2026", workflow)
        self.assertIn("os: windows-11-vs2026-arm", workflow)
        self.assertIn("generator: Visual Studio 18 2026", workflow)
        self.assertIn("tools/release/package_source.py", workflow)
        self.assertIn("--source-root .", workflow)
        self.assertIn('test "${#assets[@]}" -eq 33', workflow)
        self.assertIn(
            "AOAHID_RELEASE_VERSION: ${{ needs.validate.outputs.version }}", workflow
        )
        self.assertIn('os.environ["AOAHID_RELEASE_VERSION"]', workflow)
        self.assertIn(
            'Environment.GetEnvironmentVariable("AOAHID_RELEASE_VERSION")', workflow
        )
        self.assertNotIn("AOAHID_VERSION_MAJOR == 0", workflow)
        self.assertNotIn("AOAHID_VERSION_MAJOR != 0", workflow)
        self.assertIn('if ! response="$(gh api --paginate --slurp', workflow)
        self.assertEqual(workflow.count("verify_exact_asset_set"), 2)
        self.assertIn("public_release_matches()", workflow)
        self.assertIn("for publish_read_attempt in $(seq 1 5)", workflow)

    def test_all_workflow_actions_are_commit_pinned(self) -> None:
        workflows = sorted((REPOSITORY / ".github/workflows").glob("*.yml"))
        self.assertTrue(workflows)
        for workflow_path in workflows:
            workflow = workflow_path.read_text(encoding="utf-8")
            action_references = re.findall(
                r"^\s*uses:\s*([^\s#]+)", workflow, re.MULTILINE
            )
            self.assertTrue(action_references, workflow_path.name)
            for reference in action_references:
                with self.subTest(workflow=workflow_path.name, reference=reference):
                    self.assertRegex(reference, r"^[^@]+@[0-9a-f]{40}$")

    def test_doxygen_workflows_create_the_nested_output_directory(self) -> None:
        for name in ("ci.yml", "docs.yml"):
            workflow = (REPOSITORY / ".github" / "workflows" / name).read_text(
                encoding="utf-8"
            )
            create_at = workflow.index("mkdir -p build/doxygen")
            doxygen_at = workflow.index("doxygen tools/docs/Doxyfile", create_at)
            self.assertLess(create_at, doxygen_at, name)

    def test_doxygen_1_9_1_markdown_avoids_scoped_empty_call_autolinks(self) -> None:
        """Doxygen 1.9.1 treats scoped empty calls in code spans as links."""
        markdown_inputs = [REPOSITORY / "CHANGELOG.md"]
        markdown_inputs.extend(sorted((REPOSITORY / "docs").glob("*.md")))
        scoped_empty_call = re.compile(
            r"`[^`\n]*::[A-Za-z_][A-Za-z0-9_]*\(\)[^`\n]*`"
        )
        for markdown_path in markdown_inputs:
            with self.subTest(path=markdown_path.relative_to(REPOSITORY)):
                self.assertNotRegex(
                    markdown_path.read_text(encoding="utf-8"), scoped_empty_call
                )

    def test_installed_config_has_an_explicit_success_baseline(self) -> None:
        config = (REPOSITORY / "cmake" / "aoahid-config.cmake.in").read_text(
            encoding="utf-8"
        )
        baseline_at = config.index("set(aoahid_FOUND TRUE)")
        component_check_at = config.index("check_required_components(aoahid)")
        self.assertLess(baseline_at, component_check_at)

    def test_static_installed_target_name_is_invariant_and_compatible(self) -> None:
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        config = (REPOSITORY / "cmake" / "aoahid-config.cmake.in").read_text(
            encoding="utf-8"
        )
        release = (REPOSITORY / ".github/workflows/release.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "set_target_properties(aoahid_static PROPERTIES EXPORT_NAME aoahid_static)",
            cmake,
        )
        self.assertNotIn("EXPORT_NAME aoahid)", cmake)
        self.assertIn(
            "add_library(aoahid::aoahid ALIAS aoahid::aoahid_static)", config
        )
        self.assertIn("set(_AOAHID_BUILT_SHARED @AOAHID_BUILD_SHARED@)", config)
        self.assertIn("set(_AOAHID_BUILT_STATIC @AOAHID_BUILD_STATIC@)", config)
        self.assertIn("if(_AOAHID_BUILT_SHARED AND", config)
        self.assertIn("if(_AOAHID_BUILT_STATIC AND", config)
        self.assertEqual(
            release.count("-DAOAHID_CONSUMER_TARGET=aoahid::aoahid_static"), 3
        )
        for expected in (
            '"build/$variant"',
            '"build/stage-$variant"',
            "shared package component exports are contaminated",
            "static package component exports are contaminated",
        ):
            self.assertIn(expected, release)
        self.assertNotIn("CMakeCache.txt", release)
        self.assertNotIn("wrong AOAHID_BUILD_SHARED value", release)
        self.assertNotIn("wrong AOAHID_BUILD_STATIC value", release)

    def test_component_contract_test_is_architecture_independent(self) -> None:
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        contract = (
            REPOSITORY / "tests" / "cmake" / "package_component_contract.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn("project(aoahid_component_contract LANGUAGES NONE)", contract)
        self.assertNotIn("AOAHID_GENERATOR_PLATFORM", contract)
        self.assertNotIn("AOAHID_GENERATOR_TOOLSET", contract)
        self.assertIn("-DAOAHID_PROJECT_VERSION=${PROJECT_VERSION}", cmake)
        self.assertIn(
            "-DAOAHID_PROJECT_HOMEPAGE_URL=${PROJECT_HOMEPAGE_URL}", cmake
        )

    def test_ci_pkg_config_check_derives_the_project_version(self) -> None:
        workflow = (REPOSITORY / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("project_version = validate_release.source_versions", workflow)
        self.assertEqual(workflow.count("expected_version=project_version"), 2)
        self.assertNotRegex(workflow, r'expected_version="0\.1\.[0-9]+"')

    def test_support_table_check_is_run_only_with_a_catalog(self) -> None:
        for workflow_path in sorted((REPOSITORY / ".github/workflows").glob("*.yml")):
            for line in workflow_path.read_text(encoding="utf-8").splitlines():
                if "generate-support-table/generate.py" in line:
                    self.assertIn("--catalog", line, workflow_path.name)
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            "--catalog ${CMAKE_CURRENT_BINARY_DIR}/support-manifests.tsv", cmake
        )

    def test_repository_metadata_is_consistent(self) -> None:
        validate_release.validate_repository_identity(REPOSITORY)

if __name__ == "__main__":
    unittest.main()
