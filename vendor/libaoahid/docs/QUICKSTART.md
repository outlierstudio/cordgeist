# Quickstart: verifying against a real phone

This walkthrough takes you from a fresh checkout to a phone reacting to real
HID reports.

## 1. Install build tools

Debian/Ubuntu:

```sh
sudo apt install cmake g++ libusb-1.0-0-dev
```

Arch/CachyOS/Manjaro:

```sh
sudo pacman -S cmake gcc libusb
```

Windows: install CMake, a recent Visual Studio (or the Build Tools) with the
C++ workload, and libusb 1.0.30 or newer (for example through vcpkg;
`vcpkg.json` in this repository declares the dependency).

libusb 1.0.30 or newer is required on every platform.

## 2. Cable and hub check (the most common silent failure)

A cable or hub port marked with a battery/lightning-bolt icon is often
**charge-only** and has no data lines connected at all; the phone will never
appear in USB enumeration through it, no matter what the code does. Coming
from a hub, use a port explicitly labeled a data port (for example "USB 3.0"),
not a "PD"/charging-only port meant for feeding power into the hub itself. If
in doubt, plug the phone directly into a USB port on the computer, skipping
any hub, for the first attempt. See "Troubleshooting" below for how to
confirm the phone is even visible before touching this library at all.

## 3. Linux: USB permissions

Your distribution may already give the logged-in user access to the phone
(for example through `uaccess` rules shipped for MTP or ADB). If not, install
the udev rule, which tags USB devices with `uaccess`:

    sudo cp udev/51-aoahid.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules
    sudo udevadm trigger

Then reconnect the phone.

For a quick test, you can skip the udev setup and run the programs with sudo:

    sudo ./aoahid_verify_keyboard

If `aoahid_device_open` fails with `AOAHID_ERR_ACCESS` but works with `sudo`,
the problem is USB permissions; install the udev rule above.

## 4. Windows: `adb` or a manufacturer driver can block you

On Windows, libusb can use the phone only through WinUSB (or libusbK /
libusb0). Two things commonly get in the way:

- A running `adb` server holds the phone's ADB interface. Before trying
  anything else on Windows:

  ```powershell
  adb kill-server
  ```

  and close any app that might restart it (Android Studio, scrcpy, Vysor).
  To use `adb` on the same phone while your program runs, see
  [ADB on the same phone](../README.md#adb-on-the-same-phone).
- The manufacturer installed its own driver instead of WinUSB. Samsung
  devices are one example: HID can work while a Channel on the ADB interface
  fails with libusb status `-12` (`LIBUSB_ERROR_NOT_SUPPORTED`). Replacing the
  whole device's driver with WinUSB using [Zadig](https://zadig.akeo.ie/) may
  fix it, but it is not guaranteed on every phone; see the Windows section of
  [PORTING.md](PORTING.md#windows). If only HID is needed and the device opens,
  no driver change is required.

## 5. Build against the real libusb backend

`AOAHID_USE_FAKE_LIBUSB` selects the test-only libusb stand-in used by
`ctest`, which never touches real USB hardware. It defaults to `OFF`; the
`dev` and `tsan` presets turn it on. Keep it `OFF` for a real device:

```sh
cmake -S . -B build -DAOAHID_USE_FAKE_LIBUSB=OFF -DAOAHID_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cd build
```

With a multi-config generator such as Visual Studio, the programs are in
`build/Release/` instead.

## 6. Confirm the phone is visible to the host at all

Before running anything from this library:

```sh
lsusb          # Linux
```

or, on Windows, open Device Manager and look for the phone under "Portable
Devices" or similar. If it is not listed here, this library cannot see it
either -- go back to step 2.

## 7. Run the full multi-profile example

```sh
sudo ./aoahid_example_c
```

(On Linux, `sudo` is the fast path if you skipped step 3; on Windows no
elevation is normally required once the driver binding in step 4 is correct.)
A successful run prints the discovered device's bus/address/VID:PID/serial
and exits with status `0`. Every state change is brief, because the same
program also runs unattended in CI; it is not meant to be watched.

## 8. Run the human-observable verification programs

`examples/c/verify/` contains small programs meant to be watched. Each prints
what to do before it starts and pauses so you have time to react:

| Program | What to do first | What you should see |
|---|---|---|
| `aoahid_verify_keyboard` | Open a text field (Notes, a search box, anything with a text cursor) | The literal text `hello from libaoahid` typed out, three times |
| `aoahid_verify_mouse` | Nothing required | The cursor moves in a circle for a few seconds -- many phones do not show a visible cursor at all without an accessibility/DeX-style pointer mode enabled; a clean exit means the reports were accepted |
| `aoahid_verify_touch` | Have any screen open | The screen shows a left-right dragging motion, repeated five times |
| `aoahid_verify_toggle` | Unlock the phone and put a media app in the foreground (ideally a paused track) | Play/Pause, Volume Increment, Mute, and "AC New" each fire in turn; watch the media app and/or `adb shell getevent -lt` for the accessory's `/dev/input/eventN` (only Consumer Control Usages are sent -- see the comment at the top of the file for why System Control is deliberately excluded) |
| `aoahid_verify_accessory <manufacturer> <model>` | Open a text field; the two strings are your product values (Android matches them against an app's accessory filter and may show a "no app" prompt) | The phone disconnects and reconnects in AOA accessory mode (`18d1:2d00`, or `2d01` with USB debugging), one `a` is typed, and with `2d01` an ADB Channel opens and closes on the same USB handle. Unplug and replug to leave accessory mode |
| `aoahid_verify_all` | Open a text field | Touch circles, `hello world` typed, and mouse circles, one profile at a time for two rounds, then all three interleaved |

```sh
sudo ./aoahid_verify_keyboard
sudo ./aoahid_verify_touch
sudo ./aoahid_verify_mouse
sudo ./aoahid_verify_toggle
sudo ./aoahid_verify_all
sudo ./aoahid_verify_accessory "Your Company" "Your Model"
```

Drop `sudo` once USB permissions are set up (step 3), and on Windows.

Each one prints a clear error (with the field/reason from
`aoahid_last_error()`) instead of failing silently, and each returns a
nonzero exit status on failure.

## Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| Phone never appears in `lsusb` / Device Manager at all | Charge-only cable or hub port | Step 2 |
| `AOAHID_ERR_ACCESS` on Linux | Missing udev permission | Step 3, or run with `sudo` first to isolate the cause |
| Device open or Channel open fails only on Windows | `adb` holds the interface, or the phone has a manufacturer driver instead of WinUSB | Step 4 |
| Program exits `0`, nothing visible happens | No focused text field (keyboard), or you ran `aoahid_example_c`, whose changes are too brief to see | Use the programs in step 8 instead, and focus a text field first for the keyboard case |
| A physical-keyboard indicator/icon flickers but no character appears | The Android input pipeline detected the HID keyboard, but no text field was focused at that instant | Refocus a text field and rerun; this is not a library-level failure |
| `AOAHID_ERR_STALL` from the first `aoahid_node_open` | The device has no AOA 2.0 HID support (open does not check), or its kernel accepts HID requests only after `ACCESSORY_START` | Check the `protocol_version` discovery reported; otherwise try `aoahid_accessory_start` (see [PROTOCOL.md](PROTOCOL.md)) |

These programs are a quick sanity check. For the devices and profiles already
confirmed on real hardware, see [TARGET_MATRIX.md](TARGET_MATRIX.md).
