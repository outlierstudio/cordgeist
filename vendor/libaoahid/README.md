# libaoahid

[Canonical repository](https://github.com/nemarpuc/libaoahid)

Send keyboard, mouse, gamepad, touchscreen, touchpad, pen, and media-key input from a PC to
an Android device over a plain USB cable. The device needs no root, no app,
and no USB debugging: it sees an ordinary USB keyboard or touchscreen.
Windows and Linux, and macOS builds that are not yet tested with a device;
usable from C, C++, Python, C#, and Rust. It sends input only: nothing comes
back from the phone. The phone must support AOA version 2, which discovery
reports.

Sister library of [`libmimhid`](https://github.com/nemarpuc/libmimhid), which
sends the same kind of input to any USB target (a PC, a console, and so on)
through a microcontroller board, with no Android phone needed. The C ABI and
C++ header follow the same shape, so code moves between the two with few
changes; see [Moving between libaoahid and libmimhid](https://github.com/nemarpuc/libmimhid#moving-between-libaoahid-and-libmimhid).

https://github.com/user-attachments/assets/f6a93ffe-34ea-438b-a09f-d6c55a1f5e23

`libaoahid` is a C++20 library with a stable C ABI. It drives the HID feature
of [Android Open Accessory 2.0](https://source.android.com/docs/core/interaction/accessories/aoa2)
over libusb. You describe each input device (ranges, bit widths, Report IDs,
Usages), starting from initializers that fill in values that work with a
current Android phone; the library builds a valid HID descriptor, registers it
with Android, and keeps the report state machines (multi-touch contacts,
key rollover, pointer deltas) correct. A required value left zero is refused,
never guessed. MIT licensed.

For an overview of how the library works and why it was built, see
[the introduction on DEV](https://dev.to/nemarpuc/libaoahid-a-c-abi-library-for-sending-hid-input-to-android-over-usb-1h6f).

## Hardware status

Tested on a Samsung Galaxy Tab S11 and a POCO F6 Pro, from Windows 10 x64 and
Arch Linux:

| Input | Tested on hardware |
|---|---|
| Touchscreen (multi-touch) | yes |
| Touchpad (slide, click) | yes |
| Keyboard (full NKRO) | yes |
| Mouse (move, buttons, wheel) | yes |
| Gamepad (sticks, buttons, D-pad) | yes |
| Media keys (volume, play/pause, next/previous) | yes |
| Pen (stylus, buttons, eraser, pressure, tilt) | yes |
| Digitizer (stylus, buttons, eraser, pressure, tilt) | yes |
| Battery report (on a keyboard) | registered; level not read back |
| Raw report | not yet |

Details per device and OS are in [TARGET_MATRIX.md](docs/TARGET_MATRIX.md).

## Build

Requirements: CMake 3.20+, a C++20 compiler, and libusb 1.0.30 or later
(1.0.x only; other major/minor versions are rejected at load time). Shared and
static libraries are selected with explicit switches. Prebuilt archives are on
the [Releases](https://github.com/nemarpuc/libaoahid/releases) page. On
Windows the phone also needs a WinUSB-class driver; see
[Windows drivers](#windows-drivers).

```sh
cmake -S . -B build -DAOAHID_BUILD_SHARED=ON -DAOAHID_BUILD_STATIC=ON
cmake --build build --config Release
cmake --install build --prefix staging --config Release
```

For deterministic transport tests, configure with
`-DAOAHID_USE_FAKE_LIBUSB=ON -DAOAHID_BUILD_TESTS=ON`, build, and then run
`ctest --test-dir build -C Release --output-on-failure`. The fake backend is
test code and is never enabled in a release build.

`AOAHID_BUILD_TESTS` and `AOAHID_BUILD_EXAMPLES` are off by default.
`AOAHID_WERROR` (on by default) makes compiler warnings errors in the library, the tests and the
ABI checks; turn it off when a newer compiler than CI's warns about code that is correct.
`examples/CMakeLists.txt` also builds the examples against an installed package on its own.

### Sanitizers and allocation checks

`cmake --preset tsan` (then build and `ctest --preset tsan`) builds the library,
tests, and examples with ThreadSanitizer against the fake libusb backend;
[PORTING.md](docs/PORTING.md#threadsanitizer) lists what it requires. Use a
normal optimized build for timing. The test suite also checks that a prewarmed
caller-poll keyboard update/submit/completion cycle performs no C++ heap
allocation and takes no Context or Node mutex. These are host-side checks, not
end-to-end USB latency numbers; see [LATENCY.md](docs/LATENCY.md).

## Use

New to a real device? [QUICKSTART.md](docs/QUICKSTART.md) is a step-by-step
walkthrough covering cables and hubs, Linux udev permissions, the Windows `adb`
conflict, and small programs under [examples/c/verify/](examples/c/verify)
built to be watched (typing text, dragging on the touchscreen, moving the mouse
in circles), so you can confirm a real phone reacts before writing product
code. The complete C session, with reconnection and threads, is
[examples/c/multi_profile.c](examples/c/multi_profile.c); for a single profile in
isolation, [examples/c/profiles/](examples/c/profiles) has one self-contained
program per HID profile kind and one for the battery report, all ten of which
run without a device.

In outline: create a context, discover and select a device, open it in its
current USB mode (or first switch it with `aoahid_accessory_start`), build
immutable specs and register each as its own HID node, update state and submit
reports, then close nodes, device and context. Every step is an explicit call;
the library never waits for re-enumeration and never retries. The steps in
order, with their requests, are in [API.md](docs/API.md#open-sequence). Typing
one key on the first phone found, with the error checks left out (every call
returns an `aoahid_result`):

```c
#include "aoahid.h"

int main(void) {
    aoahid_context_options context_options;
    aoahid_context_options_init(&context_options);
    aoahid_context* context = NULL;
    aoahid_context_create(&context_options, &context);

    aoahid_discovery* discovery = NULL;
    aoahid_discover(context, AOAHID_CONTROL_TIMEOUT_MS, &discovery);
    aoahid_device_options device_options;
    aoahid_device_options_init(&device_options);
    aoahid_device* device = NULL;
    aoahid_device_open(context, aoahid_discovery_get(discovery, 0), &device_options, &device);
    aoahid_discovery_destroy(discovery);

    aoahid_keyboard_options keyboard;
    aoahid_keyboard_options_init(&keyboard);
    aoahid_spec* spec = NULL;
    aoahid_spec_create_keyboard(&keyboard, &spec);
    aoahid_node_options node_options;
    aoahid_node_options_init(&node_options);
    aoahid_node* node = NULL;
    aoahid_node_open(device, spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS, &node);
    aoahid_spec_release(spec); /* the Node keeps its own reference */
    /* Give Android about 100 ms to register the keyboard before the first report. */

    aoahid_keyboard_key(node, 0x04, 1); /* Keyboard a */
    aoahid_node_submit_blocking(node, AOAHID_SEND_TIMEOUT_MS);
    aoahid_keyboard_key(node, 0x04, 0);
    aoahid_node_submit_blocking(node, AOAHID_SEND_TIMEOUT_MS);

    aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    return 0;
}
```

The C header is [include/aoahid.h](include/aoahid.h). The optional C++ header
forwards every C function under namespace `aoahid` and adds functions that
return initialized options, plus owning pointers for Specs and discovery
snapshots. Python ctypes, C# P/Invoke, and Rust `-sys` plus typed wrappers live
under `bindings/`; they preserve the C layout. The initializers are inline C,
so a binding sets every field itself ([docs/API.md](docs/API.md#initial-values)
lists the values).

Every wait is an argument of the call that waits, and a required value or
timeout of zero is refused with `AOAHID_ERR_UNSET_FIELD`
([API.md](docs/API.md#timeouts)). `aoahid_device_open` never switches modes;
devices whose firmware accepts AOA HID requests only after `ACCESSORY_START`
are reached with `aoahid_accessory_start`
([API.md](docs/API.md#switching-a-device-to-accessory-mode),
[PROTOCOL.md](docs/PROTOCOL.md)).

## ADB on the same phone

Both `adb` and libaoahid want the phone's USB interfaces. On Windows a USB
device can be opened by only one process, so `adb` and a libaoahid application
cannot both hold the same phone. The companion library
[aoahid_adb_proxy](https://github.com/nemarpuc/aoahid_adb_proxy) solves this
from inside your application: it opens the phone's ADB interface as a Channel on
the application's own USB handle and serves it on a loopback TCP port. Then

```sh
adb connect 127.0.0.1:6555
```

reaches the phone while HID reports keep using endpoint 0. Keep the port
outside 5555-5585, which the adb server scans for emulators. Without the proxy,
run `adb kill-server` before starting your program (see
[QUICKSTART.md](docs/QUICKSTART.md#4-windows-adb-or-a-manufacturer-driver-can-block-you)).
On Linux an `adb` server rarely blocks libaoahid. The Channel calls themselves
are in [API.md](docs/API.md#bulk-channels).

## Windows drivers

On Windows, libusb can use a phone only through WinUSB (or libusbK / libusb0).
HID alone (control transfers) worked with Samsung's own driver on the Galaxy
Tab S11, but its ADB interface (a bulk interface) did not until the whole
device was switched to WinUSB; the POCO F6 Pro had WinUSB from the start. If a
phone cannot be opened, or you open a bulk interface, replacing the whole
device's driver with WinUSB in [Zadig](https://zadig.akeo.ie/) may fix it. That
is not guaranteed on every phone. The full story is in
[PORTING.md](docs/PORTING.md#windows).

## Profiles

The library builds nine kinds of HID profile, plus a battery report that any
of them can carry. The table classifies each one and says what a target still
has to confirm; what has been tested on real phones is in
[Hardware status](#hardware-status).

<!-- profile-table:start -->
| Profile | Manifest status | Input | Output | Feature transport | Qualification |
|---|---|---:|---:|---:|---|
| Keyboard, full NKRO bitmap | conditional | yes | no | no | One-bit Variable field per key; target matrix still required |
| Mouse / relative pointer | portable candidate | yes | no | no | Target matrix still required |
| Toggle: Consumer Control fields | conditional | yes | no | no | Sparse allow-list, explicit HUT semantics, and target event evidence |
| Toggle: System Control fields | conditional | yes | no | no | Explicit HUT semantics; system handling and target mapping vary |
| Gamepad | portable candidate | yes | no | no | Caller-declared axes and target mappings |
| Touchscreen, fixed MT | portable candidate | yes | no | no | Audited Linux commit and dated Android documentation; target matrix still required |
| Touchpad | conditional | yes | no | no | Distinct Linux input property (INPUT_PROP_POINTER); Android converts contacts to mouse-source motion, gesture value-add is OEM/release dependent |
| Pen, direct screen | portable candidate | yes | no | no | Invert tool transition; target matrix still required |
| Digitizer | conditional | yes | no | no | Target classification and mapping evidence required |
| Toggle: Camera keys | conditional | yes | no | no | HUT Camera Auto-focus/Shutter subset only |
| Toggle: Telephony keys | conditional | yes | no | no | Caller allow-list and target mapping required |
| Battery report (shown on a Keyboard) | conditional | yes | no | no | Second Input report inside any generated profile; Linux registers a battery only with an input device |
| Validated raw descriptor | unknown | yes | no | no | Explicit no-Android-support acknowledgement |
<!-- profile-table:end -->

"Manifest status" is the library's own classification of how portable a
descriptor shape is across Android targets; it is reported at runtime by the
Spec and Node manifests. The table is generated from those manifests by
`tools/generate-support-table/generate.py`. Hardware results are in
[Hardware status](#hardware-status).

### What the library handles for you

Each profile derives the report mechanics from its immutable Spec; the product
and target values are what its options say (the `*_init` functions fill in one
working set).

| Profile | Derived by the library | You specify |
|---|---|---|
| Keyboard | Modifier routing and a full N-Key Rollover bitmap: every declared key has its own bit, so any number of simultaneous keys is reported. | Key Usage range, Report ID, layout, locale, IME behavior. |
| Mouse | Signed 64-bit pending totals for X, Y, Wheel, and AC Pan, split into in-range report fragments; a fragment is consumed only when its transfer completes. | Axis ranges and widths, button count, Wheel/Pan presence, Report ID. |
| Consumer, System, Camera, Telephony | One allow-listed field per press; press/release/tap produce the `1` then `0` sequence each HID control type expects. | Each allowed Usage and its semantics, Report ID. |
| Gamepad | D-pad directions become the eight Hat values plus Null (`15`); adjacent pairs become diagonals, opposite pairs are rejected. Raw four-bit D-pad and no-D-pad modes are also available. | Axes (role, range, width, neutral), buttons, D-pad form, Report ID. |
| Touchscreen / touchpad | Contact Count, multi-report frames, Tip=0 release records with stable Contact IDs, Scan Time, packet sequencing. | Coordinate ranges, maximum contacts, contacts per report, optional fields. |
| Pen, Digitizer | Tip/pressure consistency, out-of-range normalization, automatic leave-and-re-enter when switching pen/eraser. | Coordinate and optional-field ranges, hover/eraser/barrel options. |
| Battery report | A second Input report inside any profile, sent only when the level changed and no input report is waiting. | Range, width, both Report IDs. |
| Raw Input | Only length, Report ID, and padding are checked against your descriptor. | Descriptor and report bytes. |

A state change is kept until its report is accepted; a conflicting change
before then returns `AOAHID_ERR_BUSY` instead of being merged. See
[PROFILES.md](docs/PROFILES.md) for the exact contract of each profile.

## Releases

A pushed `v*` tag runs the gate before creating a GitHub Release. The release
matrix produces and architecture-checks:

- Linux x86_64 and AArch64 shared objects and static archives;
- Windows x86_64 and ARM64 DLLs, import libraries, and static archives;
- macOS arm64 and x86_64 dynamic libraries and static archives;
- headers, relocatable CMake/pkg-config metadata, notices, checksums, and an
  SPDX SBOM.

For version `X.Y.Z`, the GitHub Release contains exactly 33 uploaded assets:

- twelve archives: `libaoahid-X.Y.Z-linux-{x86_64,aarch64}-ubuntu22.04-{shared,static}.tar.gz`,
  `libaoahid-X.Y.Z-macos-{arm64,x86_64}-{shared,static}.tar.gz`, and
  `libaoahid-X.Y.Z-windows-{x86_64,arm64}-{shared,static}.zip`;
- twelve matching `.spdx.json` sidecars;
- six runtime bundles: the same six target names with `-runtime.tar.gz` on
  Linux and macOS and `-runtime.zip` on Windows;
- the deterministic tagged-tree archive `libaoahid-X.Y.Z-source.tar.gz`;
- `release-manifest.json` and `SHA256SUMS`.

GitHub also displays its automatically generated source-code links separately;
the uploaded `*-source.tar.gz` is the release workflow's byte-validated archive
and is covered by `release-manifest.json` and `SHA256SUMS`.

The complete `*-shared` or `*-static` archive is what you build against: it is
the only asset carrying headers, the import library, and CMake/pkg-config
metadata. The `*-runtime` bundle is for deploying next to an application that is
already built. It carries the libaoahid shared library, the libusb runtime it
loads, and the full license set for both - `LICENSE`, `NOTICE`,
`THIRD_PARTY_NOTICES.md`, libusb's LGPL-2.1 text, and libusb's official
corresponding source archive. No bare `.so` or `.dll` is published on its own,
because an asset that ships the libusb runtime has to carry libusb's license and
corresponding source in the same distribution unit.

Platform baselines: Linux archives require at most glibc 2.35
([PORTING.md](docs/PORTING.md#release-baseline)); macOS archives target macOS 11,
are signed ad hoc and not notarized, and have not been run against a phone
([PORTING.md](docs/PORTING.md#macos)); Windows archives use the MSVC dynamic
runtime (`/MD`), so deploy the architecture-matching Visual C++ v14
Redistributable as well as the libusb runtime included next to the DLL. The
Python wheel and NuGet package contain no native libraries, so install a
version-matched shared archive as well. How the release workflow and registry
publication work is in [INTERNALS.md](docs/INTERNALS.md#version-and-release).

## Documentation

| Document | Contents |
|---|---|
| [QUICKSTART.md](docs/QUICKSTART.md) | First run on a real device: cables, permissions, drivers, verification programs |
| [API.md](docs/API.md) | C API reference: objects, options, errors, every function |
| [PROFILES.md](docs/PROFILES.md) | Descriptor and state-machine contract of each profile |
| [EXAMPLES.md](docs/EXAMPLES.md) | Walkthrough of the example programs in C, C++, Python, C#, and Rust |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Object model, threading, send path, source layout |
| [INTERNALS.md](docs/INTERNALS.md) | Implementation notes for contributors: ownership, locks, teardown, descriptor generation, release tooling |
| [PROTOCOL.md](docs/PROTOCOL.md) | How AOA 2.0 HID works and how Android handles the device |
| [LIMITS.md](docs/LIMITS.md) | Numeric limits and validation rules |
| [LATENCY.md](docs/LATENCY.md) | Latency behavior, tuning, and what the tests measure |
| [PORTING.md](docs/PORTING.md) | Platform notes: Linux udev, Windows drivers, macOS, other hosts |
| [TARGET_MATRIX.md](docs/TARGET_MATRIX.md) | Hardware test results |

## License

`libaoahid` itself is **MIT** licensed. The full text is in
[LICENSE](LICENSE); the SPDX identifier is `MIT`. The Python and C# bindings
carry the same MIT license and ship a copy of that file inside their packages.

libusb is a **separate work under LGPL-2.1-or-later**. It is never statically
absorbed into `libaoahid`: every release links it dynamically, including the
archives labelled `static` (that label describes `libaoahid` itself, not
libusb). Each binary archive therefore also carries libusb's own license and
its official source tarball under
`share/doc/libaoahid/third-party/`.

| What you receive | License | Where the text is |
|---|---|---|
| `libaoahid` library, headers, bindings, examples | MIT | `LICENSE` |
| `libusb-1.0` runtime shipped in release archives and runtime bundles | LGPL-2.1-or-later | `share/doc/libaoahid/third-party/libusb-copyright` |
| vcpkg port files bundled in Windows archives | MIT (Microsoft) | `share/doc/libaoahid/third-party/vcpkg-LICENSE.txt` |

Every first-party source file carries its own
`SPDX-License-Identifier: MIT` and copyright line, so a single file stays
identifiable after it is copied out of this repository;
`tools/check_license_headers.py` enforces that in CI. The SPDX sidecar in each
release states the license of every packaged file individually rather than
`NOASSERTION`. Redistributing a release archive unchanged satisfies both
licenses as shipped. If you relink against a modified libusb, the LGPL
obligations apply to that copy. [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) record the exact versions and
checksums.

## AI disclosure

Most of the code, tests, and documentation in this repository were written by
an AI coding assistant. Architecture and design decisions, hardware
verification steps, debugging, and review of the AI's output were done by a
human. Hardware results are listed in
[TARGET_MATRIX.md](docs/TARGET_MATRIX.md); review the code before relying on
it for anything security- or safety-relevant.
