#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Different feature builds share executable names even when Cargo's artifact
# hashes differ. Keep fake executables out of the production target forever.
production_target=$(realpath -m "${CARGO_TARGET_DIR:-$PWD/target}")
fake_target=$(realpath -m "$PWD/build/target-fake")
[[ "$production_target" != "$fake_target" ]] || {
  echo 'Production and fake Cargo target directories must differ.' >&2
  exit 2
}
export CARGO_TARGET_DIR="$production_target"
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --no-default-features --locked -- -D warnings
cargo test --workspace --no-default-features --locked
./scripts/generate-docs.sh --check

production_sha=$(sha256sum "$production_target/debug/cordgeist")
# SOFTWARE evidence only: official rmcp/dispatcher tests use the fake feature.
# Explicit missing path disables ambient host ADB discovery. Tests that need
# a supervised fake ADB provide their own fixture executable explicitly.
no_host_adb="$fake_target/no-host-adb"
[[ ! -e "$no_host_adb" ]] || { echo 'Expected absent software-test ADB path.' >&2; exit 2; }
CARGO_TARGET_DIR="$fake_target" CORDGEIST_ADB="$no_host_adb" cargo test --workspace --features fake --locked
[[ "$(sha256sum "$production_target/debug/cordgeist")" == "$production_sha" ]] || {
  echo 'Production executable changed during isolated fake tests.' >&2
  exit 1
}
echo 'Production executable unchanged after isolated SOFTWARE FAKE tests.'
