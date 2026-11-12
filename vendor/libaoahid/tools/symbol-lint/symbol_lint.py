#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Validate the public C ABI and, optionally, a built shared library.

This is intentionally a dependency-free release gate.  It compares the names
declared with AOAHID_API in include/aoahid.h with the dynamic export table and
checks that profile/product policy is not supplied by convenience factories.
Documented C-API transport-tuning fallbacks are outside that factory check.
"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


EXPORT_RE = re.compile(
    r"AOAHID_API\s+[^;]*?\bAOAHID_CALL\s+(aoahid_[A-Za-z0-9_]+)\s*\(",
    re.MULTILINE | re.DOTALL,
)
STRUCT_RE = re.compile(
    r"typedef\s+struct\s+(aoahid_[A-Za-z0-9_]+)\s*\{(.*?)\}\s*\1\s*;",
    re.MULTILINE | re.DOTALL,
)
SPEC_FACTORY_RE = re.compile(r"^aoahid_spec_create_[A-Za-z0-9_]+$")
BANNED_CONVENIENCE_RE = re.compile(r"(?:^|_)(?:default|preset|standard|typical)(?:_|$)")


class LintFailure(RuntimeError):
    pass


def read_public_api(header: Path) -> tuple[str, set[str]]:
    text = header.read_text(encoding="utf-8")
    names = set(EXPORT_RE.findall(text))
    if not names:
        raise LintFailure(f"no AOAHID_API declarations found in {header}")

    if re.search(r"#\s*include\s*[<\"][^>\"]*libusb", text, re.IGNORECASE):
        raise LintFailure("the public header exposes a libusb include")

    convenience = sorted(name for name in names if BANNED_CONVENIENCE_RE.search(name))
    if convenience:
        raise LintFailure(
            "public API contains forbidden implicit-convenience names: "
            + ", ".join(convenience)
        )

    for name, body in STRUCT_RE.findall(text):
        if not (name.endswith("_options") or name == "aoahid_capability_manifest"):
            continue
        first = re.search(r"^\s*([^;]+;)", body)
        if first is None or not re.fullmatch(
            r"\s*uint32_t\s+struct_size\s*;\s*", first.group(1)
        ):
            raise LintFailure(f"{name}: first member must be uint32_t struct_size")

    return text, names


def run_checked(argv: list[str]) -> str:
    result = subprocess.run(argv, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        command = " ".join(argv)
        raise LintFailure(
            f"command failed ({result.returncode}): {command}\n{result.stdout}{result.stderr}"
        )
    return result.stdout


def linux_exports(binary: Path) -> set[str]:
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if nm is None:
        raise LintFailure("neither nm nor llvm-nm is available")
    output = run_checked([nm, "-D", "--defined-only", str(binary)])
    exports: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[-1].split("@", 1)[0]
        if re.fullmatch(r"AOAHID_[0-9]+\.[0-9]+", name):
            continue
        exports.add(name)
    return exports


def windows_exports(binary: Path) -> set[str]:
    llvm_readobj = shutil.which("llvm-readobj")
    if llvm_readobj:
        output = run_checked([llvm_readobj, "--coff-exports", str(binary)])
        return set(re.findall(r"^\s*Name:\s*([^\s]+)\s*$", output, re.M))

    dumpbin = shutil.which("dumpbin")
    if dumpbin:
        output = run_checked([dumpbin, "/nologo", "/exports", str(binary)])
        return set(
            re.findall(r"^\s*[0-9]+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)\s*$", output, re.M)
        )
    raise LintFailure("neither llvm-readobj nor dumpbin is available")


def macos_exports(binary: Path) -> set[str]:
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if nm is None:
        raise LintFailure("neither nm nor llvm-nm is available")
    # Darwin nm's -g also includes N_PEXT (private external) symbols.  The -m
    # form exposes that distinction, so this gate compares only true external
    # and weak-external definitions from the Mach-O symbol table.  -U excludes
    # undefined imports. Mach-O C symbols have one ABI decoration underscore.
    output = run_checked([nm, "-gmU", str(binary)])
    return parse_macos_nm_exports(output)


def parse_macos_nm_exports(output: str) -> set[str]:
    exports: set[str] = set()
    for line in output.splitlines():
        if (
            "private external" in line
            or "non-external" in line
            or "external automatically hidden" in line
        ):
            continue
        if re.search(r"\b(?:weak )?external\b", line) is None:
            continue
        direct = line.split(" (for ", 1)[0]
        fields = direct.split()
        if not fields:
            continue
        name = fields[-1]
        if name.startswith("_aoahid_"):
            name = name[1:]
        exports.add(name)
    return exports


def dynamic_exports(binary: Path) -> set[str]:
    if binary.suffix.lower() == ".dll" or os.name == "nt":
        return windows_exports(binary)
    if binary.suffix.lower() == ".dylib" or sys.platform == "darwin":
        return macos_exports(binary)
    return linux_exports(binary)


def compare_exports(declared: set[str], actual: set[str]) -> None:
    missing = sorted(declared - actual)
    extra = sorted(actual - declared)
    if missing or extra:
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if extra:
            details.append("undeclared: " + ", ".join(extra))
        raise LintFailure("dynamic export table differs from aoahid.h (" + "; ".join(details) + ")")


def check_zeroed_factories(
    binary: Path, names: set[str], dll_directories: list[Path]
) -> None:
    # A deliberately oversized, aligned all-zero object is safe for every current
    # options struct. Passing a pointer (rather than NULL) catches accidental
    # profile/context acceptance without required product policy. Device open is
    # intentionally outside this factory set: only its named transport-tuning
    # fields have documented zero-value fallbacks.
    if os.name == "nt" and hasattr(os, "add_dll_directory"):
        with ExitStack() as directories:
            for directory in [binary.resolve().parent, *dll_directories]:
                if not directory.is_dir():
                    raise LintFailure(f"DLL search directory not found: {directory}")
                directories.enter_context(os.add_dll_directory(str(directory.resolve())))
            library = ctypes.CDLL(str(binary.resolve()))
    else:
        library = ctypes.CDLL(str(binary.resolve()))

    buffer_type = ctypes.c_uint64 * (4096 // ctypes.sizeof(ctypes.c_uint64))
    factories = sorted(filter(SPEC_FACTORY_RE.match, names))
    if "aoahid_context_create" in names:
        factories.insert(0, "aoahid_context_create")
    for name in factories:
        zeroed = buffer_type()
        out_spec = ctypes.c_void_p(0)
        function = getattr(library, name)
        function.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        function.restype = ctypes.c_int
        result = function(ctypes.byref(zeroed), ctypes.byref(out_spec))
        if result == 0:
            raise LintFailure(f"{name}: accepted an all-zero options struct")
        if out_spec.value is not None:
            raise LintFailure(f"{name}: wrote a non-NULL output on failure")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path, default=Path("include/aoahid.h"))
    parser.add_argument("--binary", type=Path)
    parser.add_argument(
        "--dll-directory",
        type=Path,
        action="append",
        default=[],
        help="additional dependent-DLL directory (repeatable; Windows only)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        _, declared = read_public_api(args.header)
        if args.binary is not None:
            if not args.binary.is_file():
                raise LintFailure(f"binary not found: {args.binary}")
            actual = dynamic_exports(args.binary)
            compare_exports(declared, actual)
            check_zeroed_factories(args.binary, declared, args.dll_directory)
    except (LintFailure, OSError, AttributeError) as error:
        print(f"symbol-lint: error: {error}", file=sys.stderr)
        return 1

    suffix = f" and {args.binary}" if args.binary is not None else ""
    print(f"symbol-lint: validated {len(declared)} declarations{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
