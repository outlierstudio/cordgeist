# Reproducible dependencies

Rust 1.97.1, edition 2024; install rustfmt/clippy. Project-local installation:
`source scripts/env.sh` after installing rustup under .tools/cargo and .tools/rustup.
A normal system Rust installation can instead use rust-toolchain.toml directly.
Cargo.lock is authoritative; build.rs never accesses the network.

libaoahid source: https://github.com/nemarpuc/libaoahid
commit a2458f9725ce7a6623601f999e29b6616e807bd7 (2.0.3)
tree 910fc360e427952b5d6bd96e24d95835c1214ec2
SHA256 of `git archive <commit>` (uncompressed tar):
a268faa6136f707f04248c2fdb0f709553659efca7e0be82ab9fccfeaf17aa6a.
Complete source and sibling upstream Rust bindings retained in vendor/libaoahid.
To update: fetch to a temporary checkout, verify commit and archive hash, inspect
API/lifecycle/license changes, replace tracked source, update this record, and rerun
software, native ABI, upstream fake tests and all affected hardware gates. Do not
edit the vendored library to weaken dependency checks.

`./scripts/build-native.sh` requires CMake >=3.20, C++20, Ninja, pkg-config, and
matching libusb headers/runtime >=1.0.30. Probe checks API >=0x0100010C and loaded
runtime before configure. Build uses real libusb and shared-only libaoahid, installs
under build/native-prefix (override with absolute CORDGEIST_NATIVE_PREFIX).
For user-built libusb set PKG_CONFIG_PATH and the runtime loader path explicitly;
never replace the system library. `AOAHID_LIB_DIR` selects the Rust link/runtime
location; `CORDGEIST_NATIVE_INCLUDE` selects headers. Developer artifacts retain an absolute prefix fallback. Portable artifacts use
relative executable/library RUNPATHs so an environment-cleared daemon can load
the bundled libraries on another host.

Official rmcp 3.8.0 downloaded via crates.io and verified with `cargo info`:
Apache-2.0 manifest (supplied LICENSE retains some original MIT contributions), Rust >=1.88, features server/macros/transport-io, defaults disabled.
Release checksum recorded in Cargo.lock. Its official stdio adapter is implemented with per-session private authority. SDK notices are retained under vendor/notices.
No proxy, scrcpy, OCR model, APK/DEX helper or Google platform-tools is bundled.

Production native artifacts and deterministic fake-libusb artifacts use separate build directories.

Metadata format source inspected on 2026-11-05:
https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/include/input/DisplayViewport.h
`DisplayViewport::toString()` defines INTERNAL/EXTERNAL/VIRTUAL, displayId,
orientation integer, logicalFrame, physicalFrame, deviceSize and isActive.
The parser is independent code; the fixture uses synthetic values and is not a
captured phone report. Source main may change. The adapter fails closed on
unknown/ambiguous geometry. Generic setup uses current effective wm size and
bounded rotation metadata; optional input-mapper verification is not a setup gate.
Only the recorded Note9 hardware evidence is claimed.

Discovery and Session ownership checks also run a separately built, marked native
software-fake ABI library. It is never installed or packaged; only a fresh test
process receives its loader path. For a separate ASan/UBSan build with leak checks:

```sh
CORDGEIST_DISCOVERY_SANITIZE=ON ./scripts/check-native.sh
```

The historical CORDGEIST_DISCOVERY_SANITIZE flag now covers both discovery and
Session fixtures. The compiler's ASan runtime is preloaded only for fake processes.
LeakSanitizer cannot run under ptrace; use an untraced environment and report that
limitation rather than claiming a sandbox failure is a pass. Production native
builds and their loader paths remain independent from all fake variants.
