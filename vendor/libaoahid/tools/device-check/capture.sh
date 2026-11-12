#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
# Capture the kernel, Android input classification, and event stream for a
# docs/TARGET_MATRIX.md entry. The application-API check is a separate test-app run.

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <adb-serial> </dev/input/eventN> <seconds> <output-directory>" >&2
  exit 2
fi

device_serial=$1
event_node=$2
capture_seconds=$3
output_directory=$4

if [[ ! $event_node =~ ^/dev/input/event[0-9]+$ ]]; then
  echo "event node must have the exact form /dev/input/eventN" >&2
  exit 2
fi
if [[ ! $capture_seconds =~ ^[1-9][0-9]*$ ]]; then
  echo "capture duration must be a positive integer" >&2
  exit 2
fi

mkdir -p -- "$output_directory"
adb -s "$device_serial" get-state >"$output_directory/adb-state.txt"
adb -s "$device_serial" shell getprop ro.build.fingerprint \
  >"$output_directory/build-fingerprint.txt"
adb -s "$device_serial" shell uname -a >"$output_directory/uname.txt"
adb -s "$device_serial" shell cat /proc/bus/input/devices \
  >"$output_directory/proc-bus-input-devices.txt"
adb -s "$device_serial" shell dumpsys input >"$output_directory/dumpsys-input.txt"
adb -s "$device_serial" shell getevent -lp "$event_node" \
  >"$output_directory/getevent-capabilities.txt"

set +e
timeout --signal=INT "${capture_seconds}s" \
  adb -s "$device_serial" shell getevent -lt "$event_node" \
  >"$output_directory/getevent-events.txt"
capture_status=$?
set -e
if [[ $capture_status -ne 0 && $capture_status -ne 124 && $capture_status -ne 130 ]]; then
  exit "$capture_status"
fi

printf '%s\n' "$device_serial" >"$output_directory/adb-serial.txt"
printf '%s\n' "$event_node" >"$output_directory/event-node.txt"
printf '%s\n' "$capture_seconds" >"$output_directory/capture-seconds.txt"
