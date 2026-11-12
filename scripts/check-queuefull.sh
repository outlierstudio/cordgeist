#!/usr/bin/env bash
# SOFTWARE FAKES only. Static origin diagnostics; never discovers real devices.
set -euo pipefail
cd "$(dirname "$0")/.."
# Never overwrite the executable used by production process tests.
export CARGO_TARGET_DIR="$PWD/build/target-fake"
export CORDGEIST_ADB="$CARGO_TARGET_DIR/no-host-adb"
[[ ! -e "$CORDGEIST_ADB" ]] || { echo 'Expected absent software-test ADB path.' >&2; exit 2; }
cargo test --locked --features fake --lib pairing_store::fork_regression -- --nocapture
cargo test --locked --features fake --test adb capacity_rejection -- --nocapture
cargo test --locked --features fake --test discovery outstanding_scan -- --nocapture
cargo test --locked --features fake --test discovery_service -- --nocapture
