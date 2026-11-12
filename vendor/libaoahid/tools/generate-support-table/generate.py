#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Render or verify the README table from exported runtime manifests."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Presentation:
    key: str
    name: str
    expected_status: str
    qualification: str


ROWS = (
    Presentation("keyboard", "Keyboard, full NKRO bitmap", "conditional", "One-bit Variable field per key; target matrix still required"),
    Presentation("mouse-wheel-pan", "Mouse / relative pointer", "portable candidate", "Target matrix still required"),
    Presentation("consumer-controls", "Toggle: Consumer Control fields", "conditional", "Sparse allow-list, explicit HUT semantics, and target event evidence"),
    Presentation("system-controls", "Toggle: System Control fields", "conditional", "Explicit HUT semantics; system handling and target mapping vary"),
    Presentation("gamepad", "Gamepad", "portable candidate", "Caller-declared axes and target mappings"),
    Presentation("touchscreen-fixed-mt", "Touchscreen, fixed MT", "portable candidate", "Audited Linux commit and dated Android documentation; target matrix still required"),
    Presentation("touchpad", "Touchpad", "conditional", "Distinct Linux input property (INPUT_PROP_POINTER); Android converts contacts to mouse-source motion, gesture value-add is OEM/release dependent"),
    Presentation("pen", "Pen, direct screen", "portable candidate", "Invert tool transition; target matrix still required"),
    Presentation("digitizer", "Digitizer", "conditional", "Target classification and mapping evidence required"),
    Presentation("camera-keys", "Toggle: Camera keys", "conditional", "HUT Camera Auto-focus/Shutter subset only"),
    Presentation("telephony-keys", "Toggle: Telephony keys", "conditional", "Caller allow-list and target mapping required"),
    Presentation("keyboard-battery", "Battery report (shown on a Keyboard)", "conditional", "Second Input report inside any generated profile; Linux registers a battery only with an input device"),
    Presentation("raw-validated", "Validated raw descriptor", "unknown", "Explicit no-Android-support acknowledgement"),
)

STATUS_NAMES = {
    1: "portable candidate",
    2: "conditional",
    3: "custom system only",
    4: "unsupported",
    5: "unknown",
}

START = "<!-- profile-table:start -->"
END = "<!-- profile-table:end -->"


def load_catalog(path: Path) -> dict[str, dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        required = {
            "key",
            "android_status",
            "input",
            "output",
            "feature_transport",
            "descriptor_length",
            "reports",
        }
        if reader.fieldnames is None or set(reader.fieldnames) != required:
            raise SystemExit("manifest catalog has an unexpected header")
        catalog: dict[str, dict[str, str]] = {}
        for row in reader:
            key = row["key"]
            if not key or key in catalog:
                raise SystemExit(f"duplicate or empty manifest key: {key!r}")
            catalog[key] = row

    expected_keys = {row.key for row in ROWS}
    if set(catalog) != expected_keys:
        missing = sorted(expected_keys - set(catalog))
        extra = sorted(set(catalog) - expected_keys)
        raise SystemExit(f"manifest catalog keys disagree: missing={missing}, extra={extra}")
    return catalog


def render(catalog: dict[str, dict[str, str]]) -> str:
    lines = [
        "| Profile | Manifest status | Input | Output | Feature transport | Qualification |",
        "|---|---|---:|---:|---:|---|",
    ]
    for presentation in ROWS:
        manifest = catalog[presentation.key]
        try:
            status = STATUS_NAMES[int(manifest["android_status"])]
            input_supported = int(manifest["input"])
            output_supported = int(manifest["output"])
            feature_transport = int(manifest["feature_transport"])
            descriptor_length = int(manifest["descriptor_length"])
        except (KeyError, ValueError) as error:
            raise SystemExit(f"invalid manifest values for {presentation.key}: {error}") from error
        if status != presentation.expected_status:
            raise SystemExit(
                f"{presentation.key} runtime status {status!r} disagrees with evidence policy "
                f"{presentation.expected_status!r}"
            )
        if (input_supported, output_supported, feature_transport) != (1, 0, 0):
            raise SystemExit(f"{presentation.key} violates the Input-only transport contract")
        if descriptor_length <= 0 or not manifest["reports"]:
            raise SystemExit(f"{presentation.key} has no descriptor/report manifest data")
        lines.append(
            f"| {presentation.name} | {status} | yes | no | no | {presentation.qualification} |"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--readme", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    arguments = parser.parse_args()
    catalog = load_catalog(arguments.catalog)
    text = arguments.readme.read_text(encoding="utf-8")
    before, marker, remainder = text.partition(START)
    if not marker:
        raise SystemExit("start marker missing")
    _, marker, after = remainder.partition(END)
    if not marker:
        raise SystemExit("end marker missing")
    expected = before + START + "\n" + render(catalog) + "\n" + END + after
    if arguments.check:
        return 0 if expected == text else 1
    print(expected, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
