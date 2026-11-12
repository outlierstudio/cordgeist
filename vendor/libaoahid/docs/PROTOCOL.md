# AOA 2.0 HID Protocol

Android Open Accessory (AOA) 2.0 lets a USB host act as one or more HID input
devices (keyboard, mouse, touchscreen, game controller, …) for an Android
device that is the USB peripheral. This document explains the protocol as
libaoahid uses it, and which library function sends what.

Primary sources:

- AOA 1.0: <https://source.android.com/docs/core/interaction/accessories/aoa>
- AOA 2.0: <https://source.android.com/docs/core/interaction/accessories/aoa2>
- Android kernel implementation, `drivers/usb/gadget/function/f_accessory.c`
  in the Android common kernel (<https://android.googlesource.com/kernel/common/>)
- Android input stack: <https://source.android.com/docs/core/interaction/input>
- libusb on Windows: <https://github.com/libusb/libusb/wiki/Windows>

## Control requests

All requests are vendor requests to the device on endpoint 0.
`bmRequestType` is `0xC0` (IN | vendor | device) or `0x40` (OUT | vendor |
device).

| bRequest | Name | Dir | wValue | wIndex | Data |
| --- | --- | --- | --- | --- | --- |
| 51 | ACCESSORY_GET_PROTOCOL | IN | 0 | 0 | 2 bytes: protocol version (little-endian). 0 = no AOA; 2 or more needed for HID. |
| 52 | ACCESSORY_SEND_STRING | OUT | 0 | String ID: 0 manufacturer, 1 model, 2 description, 3 version, 4 URI, 5 serial | NUL-terminated string |
| 53 | ACCESSORY_START | OUT | 0 | 0 | none |
| 54 | ACCESSORY_REGISTER_HID | OUT | HID ID (host-chosen) | Total report descriptor length | none |
| 55 | ACCESSORY_UNREGISTER_HID | OUT | HID ID | 0 | none |
| 56 | ACCESSORY_SET_HID_REPORT_DESC | OUT | HID ID | Byte offset of this fragment in the descriptor | Descriptor fragment |
| 57 | ACCESSORY_SEND_HID_EVENT | OUT | HID ID | 0 | One HID input report |

Requests 51–53 are defined in AOA 1.0; 54–57 in AOA 2.0. The kernel driver
answers request 51 with `PROTOCOL_VERSION`, which is 2 in current Android
common kernels.

## HID flow

1. **Register** (54) an ID and announce the descriptor length.
2. **Send the descriptor** (56) in one or more fragments. The kernel rejects a
   fragment unless its offset equals the bytes received so far and it stays
   within the announced length. When the last byte arrives, the kernel creates
   the HID device.
3. **Send events** (57), each one an input report as defined by the
   descriptor. The kernel rejects an ID that is not fully registered.
4. **Unregister** (55) when done. The kernel also removes the devices when the
   USB connection goes away.

Several IDs can be registered at once, each with its own descriptor.

## Accessory mode vs. the current USB mode

Requests 52 and 53 switch the phone into accessory mode: it disconnects and
re-enumerates with Google's vendor ID `0x18D1` and a product ID in
`0x2D00`–`0x2D05` (the AOA 2.0 page lists `0x2D02`–`0x2D05` for audio
variants). Accessory mode is what an Android app matching the accessory
strings needs.

HID requests 54–57 go to endpoint 0, and the kernel code handles them in the
same vendor-request handler as the other AOA requests. The driver also exports
`acc_ctrlrequest_composite`, through which a composite gadget can pass these
requests to it outside accessory mode. Whether a given device answers them in
its normal USB mode (for example MTP or ADB) depends on its gadget
configuration. We have not verified this across devices. If a device does
not, `aoahid_node_open` fails (typically with `AOAHID_ERR_STALL`), and you can
switch it to accessory mode first.

## Input only

The host sends input reports only. The kernel's HID low-level driver for these
devices has a `raw_request` that does nothing and returns 0, and AOA defines
no request that carries Output or Feature reports back to the host. So
keyboard LED state, force feedback, and Feature reports (such as a
touchscreen's maximum contact count) never reach the host. libaoahid reflects
this: capability manifests report whether Output and Feature transports are
supported, and Specs are built around Input reports.

## How Android sees the device

The kernel driver allocates a `hid_device` with bus type `BUS_USB`, gives it
the received descriptor, and adds it to the Linux HID bus. From there it is
treated like any HID device: a Linux HID driver (for example the generic or
multitouch driver) binds to it and creates input devices. Android's `EventHub`
reads those through `evdev`, and `InputReader` turns them into Android input
events. Which kernel driver binds, and how Android classifies the device,
depends on the descriptor and on that device's kernel and Android version. See
[PROFILES.md](PROFILES.md) and [TARGET_MATRIX.md](TARGET_MATRIX.md).

## Host-side caveats

- **Windows driver.** libusb needs WinUSB (or libusbK / libusb-win32) bound
  to the device. The phone's normal interfaces usually have other drivers, so
  you have to install one. The accessory-mode device
  (a different product ID) needs its own binding.
- **One process per device on Windows.** WinUSB does not allow several
  applications to open one device at once (libusb Windows wiki). Inside one
  process, libaoahid shares a single handle between the Device, its Channels,
  and discovery.
- **Linux permissions.** The user needs write access to the USB device node,
  usually through a udev rule.
- **libusb version.** The library requires libusb runtime 1.0.30 or later and
  fails `aoahid_context_create` with `AOAHID_ERR_UNSUPPORTED` otherwise.

## What libaoahid does, and where

| Step | Function | Requests |
| --- | --- | --- |
| Find AOA-capable devices | `aoahid_discover` | 51 to every accessible USB device. Lists only those with a nonzero version, with the version in `protocol_version`. The caller decides whether version 2 is required. |
| Switch to accessory mode (optional) | `aoahid_accessory_start` | 51, then 52 for each non-null string (manufacturer and model required), then 53. Returns when 53 completes. It does not wait for re-enumeration and never retries. A device already in accessory mode receives nothing. |
| Open the device | `aoahid_device_open` | None. Opens the device in its current mode and allocates the transfer pool. |
| Register a HID device | `aoahid_node_open` | 54, then 56 in fragments of `descriptor_fragment_bytes`. On failure after 54, sends a best-effort 55. |
| Send input | `aoahid_node_submit`, `aoahid_node_submit_blocking`, `aoahid_raw_submit` | 57, asynchronous. |
| Unregister | `aoahid_node_close`, `aoahid_device_close`, context destroy | Neutral reports as needed, then 55. |
| Bulk data (e.g. ADB) | `aoahid_channel_open/read/write` | No AOA request. Bulk transfers on an interface chosen by class/subclass/protocol. |

The request implementations are in `src/transport/discovery.cpp` (51 probe),
`src/transport/port.cpp` (51–53), and `src/transport/transport.cpp` (54–57).

## Hardware status

Which devices, hosts, and profiles have been confirmed on real hardware is
recorded in [TARGET_MATRIX.md](TARGET_MATRIX.md).

See also: [README.md](https://github.com/nemarpuc/libaoahid#readme), [ARCHITECTURE.md](ARCHITECTURE.md),
[QUICKSTART.md](QUICKSTART.md), [LIMITS.md](LIMITS.md).
