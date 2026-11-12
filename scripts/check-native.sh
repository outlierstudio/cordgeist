#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export AOAHID_LIB_DIR=${AOAHID_LIB_DIR:-"$PWD/build/native-prefix/lib"}
[[ -f "$AOAHID_LIB_DIR/libaoahid.so" ]] || { echo 'Run scripts/build-native.sh first' >&2; exit 1; }
# Real library and project ABI tests. No phone is opened.
cargo test --workspace --locked --features native-aoa
cargo build --locked --features native-aoa --bin native-check
# Verify from an ordinary loader environment, outside cargo's LD_LIBRARY_PATH.
env -u LD_LIBRARY_PATH ./target/debug/native-check
# Completely separate deterministic SOFTWARE FAKE build; never installed.
cmake -S vendor/libaoahid -B build/native-fake-tests -G Ninja \
  -DAOAHID_USE_FAKE_LIBUSB=ON -DAOAHID_BUILD_TESTS=ON \
  -DAOAHID_BUILD_SHARED=ON -DAOAHID_BUILD_STATIC=OFF -DAOAHID_CHECK_SERIALIZATION=ON
cmake --build build/native-fake-tests --parallel 2
ctest --test-dir build/native-fake-tests --output-on-failure

# Exercise the actual project native adapter with a marked, isolated SOFTWARE FAKE
# libusb build. The test resolves the marker before creating any Context.
sanitize=${CORDGEIST_DISCOVERY_SANITIZE:-OFF}
case "$sanitize" in
  OFF) test_build=build/native-discovery-fake ;;
  ON) test_build=build/native-discovery-fake-asan ;;
  *) echo 'CORDGEIST_DISCOVERY_SANITIZE must be ON or OFF' >&2; exit 2 ;;
esac
cmake -S native/discovery-test -B "$test_build" -G Ninja -DAOAHID_SANITIZE="$sanitize"
cmake --build "$test_build" --parallel 2
cargo test --locked --features native-aoa --lib --no-run --message-format=json > build/discovery-test-artifacts.json
test_binary=$(python3 - <<'PYCODE'
import json
from pathlib import Path
artifacts = [json.loads(line) for line in Path('build/discovery-test-artifacts.json').read_text().splitlines() if line.startswith('{')]
binaries = [a['executable'] for a in artifacts if a.get('reason') == 'compiler-artifact' and a.get('target', {}).get('name') == 'cordgeist' and a.get('executable') and a.get('profile', {}).get('test')]
if len(binaries) != 1:
    raise SystemExit('expected exactly one project library test binary')
print(binaries[0])
PYCODE
)
# Both quiet operation and refusal of externally forced native logging are
# tested in fresh processes; no unsafe process-global environment mutation.
for debug in 0 4; do
  for fixture in native::discovery::software_fake_native_abi_discovery native::session::tests::software_fake_native_abi_session; do
    if [[ $sanitize == ON ]]; then
      sanitizer_runtime=$(cc -print-file-name=libasan.so)
      [[ -f "$sanitizer_runtime" ]] || { echo 'AddressSanitizer runtime unavailable' >&2; exit 1; }
      LIBUSB_DEBUG="$debug" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        LD_PRELOAD="$sanitizer_runtime" LD_LIBRARY_PATH="$PWD/$test_build/upstream" \
        "$test_binary" --ignored --exact "$fixture" --test-threads=1
    else
      LIBUSB_DEBUG="$debug" LD_LIBRARY_PATH="$PWD/$test_build/upstream" "$test_binary" --ignored --exact "$fixture" --test-threads=1
    fi
  done
done
