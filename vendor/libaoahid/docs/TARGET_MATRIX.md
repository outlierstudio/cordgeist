# Target Matrix

This file records which combinations of host, Android device, and profile have
been confirmed on real hardware, and which have not.

## Summary

- Confirmed working on real hardware (2026-09, 2026-10): **touchscreen,
  touchpad, keyboard, mouse, gamepad, pen, digitizer, and media keys (Consumer
  Control toggle)**.
- Devices: Samsung Galaxy Tab S11 and POCO F6 Pro (HyperOS).
- Hosts: Windows 10 x64 (WinUSB and libusbK) and Arch Linux x86_64 (usbfs).
- Not yet tested on hardware: raw, and the System Control, Camera, and
  Telephony toggle forms.

"Confirmed" means the profile was driven through
[aoahid_player](https://github.com/nemarpuc/aoahid_player) or `devtest`
and the input took effect on the device, with kernel `getevent` traces
captured over the ADB proxy where noted.

## Hardware-tested combinations

| Device | Android build | Host OS / libusb backend | Date |
|---|---|---|---|
| Samsung Galaxy Tab S11 | OEM release | Windows 10 x64 / WinUSB, libusbK | 2026-09 |
| Samsung Galaxy Tab S11 | OEM release | Arch Linux x86_64 / usbfs | 2026-09 |
| Samsung Galaxy Tab S11 | OEM release (SM-X730) | Arch Linux x86_64 / usbfs | 2026-10 |
| POCO F6 Pro | HyperOS release | Windows 10 x64 / WinUSB, libusbK | 2026-09 |
| POCO F6 Pro | HyperOS release | Arch Linux x86_64 / usbfs | 2026-09 |

On Windows the Samsung tablet needed its whole-device driver replaced with
WinUSB before a Bulk Channel could open; see [PORTING.md](PORTING.md#windows).

## Per-profile status

Unit, descriptor, and golden-byte tests run in CI for every profile. The
column below is about real devices only.

| Profile | Real hardware |
|---|---|
| Touchscreen (multi-touch) | Confirmed on both devices and both hosts |
| Keyboard (full-NKRO bitmap) | Confirmed on both devices and both hosts |
| Mouse | Confirmed on both devices and both hosts |
| Gamepad, Hat switch | Confirmed on both devices and both hosts |
| Gamepad, raw D-pad fields | Confirmed on both devices and both hosts |
| Gamepad, no D-pad | Confirmed on both devices and both hosts |
| Toggle: Consumer Control (media keys) | Confirmed on both devices and both hosts |
| Toggle: System Control | Not tested |
| Toggle: Camera | Not tested |
| Toggle: Telephony | Not tested |
| Pen | Confirmed on Samsung Galaxy Tab S11 (Arch Linux; devtest + getevent) |
| Digitizer | Confirmed on Samsung Galaxy Tab S11 (Arch Linux; devtest + getevent) |
| Touchpad | Confirmed on Samsung Galaxy Tab S11 (Arch Linux; devtest + getevent) |
| Battery report (on a Keyboard) | Registered on Samsung Galaxy Tab S11 (Arch Linux; kernel 6.6.102-android15, Android 16, 2026-10-11): the kernel creates the power supply and `dumpsys input` lists the device with class `BATTERY` and one battery. The reported level was not read back: sysfs is not readable from the adb shell. A battery-only descriptor got no power supply on the same device. |
| Raw report | Not tested; the library makes no claim about how Android interprets a raw descriptor |

## Transport and accessory mode

| Behavior | Status |
|---|---|
| In the current USB mode (no `ACCESSORY_START`), the device accepts AOA HID requests 54-57 and the input takes effect | Confirmed (both devices, both hosts) |
| After `aoahid_accessory_start`, the device re-enumerates as `18d1:2d00`, or `18d1:2d01` with USB debugging on, at the same bus and port path | Confirmed (both devices, both hosts) |
| In accessory mode, requests 54-57 register the HID and deliver input as in the current USB mode | Confirmed (both devices, both hosts) |
| On Windows, `2d00`/`2d01` binds a libusb-usable driver (WinUSB or libusbK) | Confirmed (both devices) |
| `GET_PROTOCOL` (request 51) sent by discovery to an accessory-mode device does not disturb the accessory session | Not tested |
| With `2d01`, an ADB Bulk Channel and an AOA HID Node work at the same time on one handle | Not tested |
| A zero-length Bulk OUT transfer terminates a write the same way on Linux and Windows | Not tested |
| A first report that races HID registration STALLs rather than being dropped, so the caller's resend delivers it | Confirmed (Galaxy Tab S11, Arch Linux, 2026-09): request 57 STALLed for 6-16 ms after `aoahid_node_open` returned, then a resend was accepted. Reports accepted in the first milliseconds after that, up to about 60 ms in one run, did not reach `getevent`; see [API.md](API.md) step 5. |
| HID latency during a large ADB transfer on the same handle stays close to the idle case | Not tested |
| Unplugging, or `svc usb setFunctions`, returns the device from accessory mode to its normal USB functions | Not tested |

Other Android versions, OEMs, and kernels have not been tested. AOA HID support
in the current USB mode depends on the device kernel; if a device STALLs the
first `aoahid_node_open`, try `aoahid_accessory_start` (see
[PROTOCOL.md](PROTOCOL.md)).

## Build targets

Release archives are built by `.github/workflows/release.yml` for:

| Host | Runner | Artifacts |
|---|---|---|
| Linux x86_64, glibc | `ubuntu-22.04` | `libaoahid.so`, `libaoahid.a` |
| Linux aarch64, glibc | `ubuntu-22.04-arm` | `libaoahid.so`, `libaoahid.a` |
| Windows x86_64 | `windows-2025-vs2026` | `aoahid.dll` + import library, `aoahid_static.lib` |
| Windows ARM64 | `windows-11-vs2026-arm` | `aoahid.dll` + import library, `aoahid_static.lib` |
| macOS arm64 | `macos-15` | `libaoahid.dylib`, `libaoahid.a` |
| macOS x86_64 | `macos-15-intel` | `libaoahid.dylib`, `libaoahid.a` |

The macOS archives are built with the fake libusb backend's tests and an
installed-package consumer, and have not been tested with a real device or
libusb's macOS backend.

`.github/workflows/ci.yml` also builds and tests Linux musl (x86_64 and
aarch64). That is a portability check only; no release archive is published
for it, and it has not been tested with a real device.

## Checking a new device

To add a device or profile, record at least:

- device model, Android version, and build fingerprint (`adb shell getprop ro.build.fingerprint`);
- host OS, architecture, libusb version, and backend;
- the libaoahid version or commit;
- `adb shell getevent -lp` for the new input node and `adb shell getevent -lt`
  while sending input;
- the relevant `adb shell dumpsys input` block; and
- whether the input took effect in a foreground app.

`tools/device-check/` has a capture script and a small Android app for the
last three items. Keep one row per exact device/build/host combination; do not
merge results from different devices or Android versions.

Suggested cases per profile:

| Profile | Cases |
|---|---|
| Keyboard | Modifier chords; release all; more than six non-modifier keys held at once; key repeat; layout and IME differences |
| Mouse | Positive and negative X/Y; every button; wheel; AC Pan; high-rate motion; pointer capture |
| Toggle | Every allowed Usage; press and release; screen off; foreground and background media/camera/call handling |
| Gamepad | Every button and axis; neutral; minimum and maximum; all eight Hat directions and the null (centered) value, or each raw D-pad bit |
| Touchscreen | One to the configured maximum contacts; stable Contact IDs; explicit lift; edges and corners; optional pressure/azimuth fields; Scan Time when enabled |
| Touchpad | Touchscreen cases plus each button; observe the Android motion source |
| Pen, Digitizer | Hover; tip; eraser in and out; barrel buttons; pressure, tilt, and twist when enabled; four corners; pen with touch |
| Battery report | Kernel `CONFIG_HID_BATTERY_STRENGTH`; minimum, midpoint, maximum; the device's `power_supply` directory, class `BATTERY` in `dumpsys input`, and `InputDevice.getBatteryState()` from an app |
| Teardown | Neutral report; close; unplug with a report in flight; reopen; Android reboot |
