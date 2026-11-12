# Porting

The public ABI is platform-neutral. Only `src/transport` includes libusb.

## Linux

Build against libusb 1.0.30 or newer. At run time, `aoahid_context_create`
also checks `libusb_get_version()` and accepts only the 1.0 line with micro
version 30 or later; a future 1.1 or 2.x libusb is rejected until it has been
checked for compatibility.

The invoking user needs USB access to the phone. `aoahid_device_open` uses the
phone in its current USB mode, so the device keeps its own OEM VID/PID and
there is no fixed VID/PID to match. `udev/51-aoahid.rules` therefore tags USB
devices with `uaccess`, which gives the logged-in seat access; many
distributions already do this for phones through their MTP or ADB rules. The file also has a `plugdev` group
rule for Google's accessory-mode IDs (`18d1:2d00`-`2d05`), which a device uses
after `aoahid_accessory_start`. Review both rules against your distribution's
policy before installing. See the
[systemd udev documentation](https://www.freedesktop.org/software/systemd/man/latest/udev.html)
for rule syntax.

A running `adb` server rarely blocks libaoahid on Linux, but if
`aoahid_device_open` fails only while `adb devices` also lists the phone, run
`adb kill-server` and retry before assuming a permission problem.

### Release baseline

Linux release archives are built on Ubuntu 22.04 and require at most glibc
2.35. The release workflow runs `readelf --version-info --wide` over every
shared library it ships and fails if any `GLIBC_*` requirement is newer than
`GLIBC_2.35` or not numeric. Each Linux archive carries
`share/doc/libaoahid/glibc-requirements.json`. These archives are not musl or
generic-Linux builds; musl is built and tested in CI only.

### ThreadSanitizer

Linux builds with GCC 12 or newer, or Clang, can enable ThreadSanitizer:

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

The preset sets `AOAHID_TSAN=ON`, uses the test-only fake libusb backend and a
`RelWithDebInfo` build, and runs tests with `TSAN_OPTIONS=halt_on_error=1`.
CMake rejects `AOAHID_TSAN` on non-Linux hosts, with other compilers, with GCC
11 or older, and together with `AOAHID_SANITIZE` or `AOAHID_BUILD_FUZZ`. GCC 11
is rejected because of a false condition-variable report
([GCC bug 101978](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101978)); CI
uses GCC 12 with every report enabled. This is a host concurrency check, not
a release configuration.

## Windows

libusb can open a device only when Windows has bound WinUSB, libusbK, or
libusb0 to it; any other driver makes the claim fail with
`LIBUSB_ERROR_NOT_SUPPORTED` (libusb status `-12`). See the
[libusb Windows wiki](https://github.com/libusb/libusb/wiki/Windows).

AOA control requests go to the device, not to a particular interface. Unless
the caller names and claims an interface, libaoahid lets libusb pick a usable
one, and it releases only a claim it made itself.

**A running `adb` server or a manufacturer's driver can block this.** The
Google USB Driver is WinUSB-based and is fine by itself, but while an `adb`
server holds the ADB interface, opening or claiming it fails. Before opening
the device:

```powershell
adb kill-server
```

and close Android Studio, Vysor, scrcpy, or anything else that restarts the
server. To keep `adb` working on the same phone while your application runs,
use [aoahid_adb_proxy](../README.md#adb-on-the-same-phone).

Some manufacturers bind their own driver instead of WinUSB. On a Samsung Galaxy
Tab S11 with Samsung's `dg_ssudbus` driver under Windows 10 x64, HID worked
but `aoahid_channel_open` on the ADB interface failed with
`AOAHID_ERR_UNSUPPORTED` and libusb status `-12`. Replacing only the ADB or MTP
interface's driver did not help. Replacing the whole device's driver
("SAMSUNG Android") with WinUSB in [Zadig](https://zadig.akeo.ie/) did: with
the whole device on WinUSB, libusb reaches every interface through one WinUSB
handle. The cost is losing Windows' own functions for that device, such as MTP
file transfer. To undo it, uninstall the device in Device Manager with its
driver and reconnect.

Release archives for Windows x64 and ARM64 are built on native GitHub-hosted
runners and include the import library and the libusb DLL with its license.

## macOS

Release archives for arm64 and x86_64 are built with libusb 1.0.30 from its
unmodified source, a deployment target of macOS 11, and `@loader_path` as the
run path, so `libaoahid.dylib` finds `libusb-1.0.0.dylib` in its own directory.
Both libraries are signed ad hoc and not notarized.

The macOS build is checked only with the fake libusb backend and installed-package
consumers on GitHub-hosted runners. It has not been run against a phone, so
whether libusb's macOS backend can open an Android device, claim the ADB
interface, or take it from another process is unknown here. Record a first
result as its own row in [TARGET_MATRIX.md](TARGET_MATRIX.md#checking-a-new-device).

## Other libusb platforms

The transport uses only public libusb 1.0.30 APIs, and CI builds and tests on
Linux musl. Release archives cover only the targets in
[TARGET_MATRIX.md](TARGET_MATRIX.md#build-targets). A new backend needs a check
of its control-transfer length, claim, cancel, event, and device-matching
behavior against the libusb documentation and source, plus integration tests.
Do not assume Windows or Linux behavior carries over.

## New Android targets

Accessory-side and input-framework behavior varies by kernel, Android version,
and OEM. Before relying on a new target:

1. identify its Android version, build fingerprint, and kernel version;
2. run the checks in [TARGET_MATRIX.md](TARGET_MATRIX.md#checking-a-new-device);
3. record it as its own row; results from one device or Android version do
   not carry over to another.
