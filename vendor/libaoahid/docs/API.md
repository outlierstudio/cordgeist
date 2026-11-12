# C API

`include/aoahid.h` is the stable C ABI. It exposes opaque handles and
fixed-width types only; no libusb or C++ type crosses the boundary. Each
exported function has an Ownership / Blocking / Synchronization / Returns
contract in the header; this page summarizes how the pieces fit together. For
the object model and threading internals see [ARCHITECTURE.md](ARCHITECTURE.md);
for the AOA requests themselves see [PROTOCOL.md](PROTOCOL.md).

Every result and option domain (`aoahid_result`, `aoahid_event_mode`,
`aoahid_profile_kind`, ...) is an `int32_t` typedef with integral constants, so
structure layout does not change when a compiler uses short enums.
`tests/abi/test_c_abi.c` asserts every domain width.

## Structure rules

- Every top-level options structure starts with `struct_size` and `reserved`.
  `struct_size` is `sizeof(struct_type)` and every `reserved*` field is zero.
  A size mismatch fails with `AOAHID_ERR_PARAM`, so an old binary and a new
  header never silently reinterpret a structure.
- Each options structure has an initializer, `aoahid_<structure>_init`, that
  sets `struct_size`, zeroes the rest and fills in values that work with a
  current Android phone (see [Initial values](#initial-values)). Start from it
  and change only what you need.
- A required field left zero is refused with `AOAHID_ERR_UNSET_FIELD`; the
  library never puts a value of its own in its place. Every enumeration starts
  at 1, so a zeroed enumeration field is always refused rather than read as a
  choice. Where zero is a real value (a logical minimum, a neutral axis value,
  interface class 0, a count of optional barrel buttons) the field has no
  "unset" meaning, and what is optional has its own `enable_*` flag or count.
- `enable_*`, `has_*` and the other flags are exactly 0 or 1. A disabled
  field's declaration must be all zero.

## Initial values

The initializers are `static inline` functions in `aoahid.h`, so they are not
in the binary ABI and the language bindings do not have them; a binding sets
the same fields itself. `aoahid.hpp` adds functions that return an initialized
structure (`aoahid::device_options`, ...).

| Initializer | What it fills in |
|---|---|
| `aoahid_context_options_init` | `AOAHID_EVENT_INTERNAL_THREAD`, `AOAHID_LOG_DISABLED`. |
| `aoahid_device_options_init` | 4096-byte descriptor fragments, AOA wire limit, EP0 and host control limits; 4 transfer slots; reports up to 1024 bytes; the Linux HID limits of [LIMITS.md](LIMITS.md) (4096, 256, 4, 256, 12288, 65528); `AOAHID_INTERFACE_CLAIM_NONE` with `interface_number = -1`; `validate_reports = 0`. |
| `aoahid_accessory_options_init` | Manufacturer and model `"libaoahid"`; no other string. Set your own when you ship an app that filters accessories. |
| `aoahid_channel_options_init` | The ADB interface `0xFF/0x42/0x01`, `AOAHID_CHANNEL_READ_REQUEST`, 2 OUT transfers of up to 1 MiB, zero-length termination on. |
| `aoahid_node_options_init` | No reserved slot. |
| `aoahid_keyboard_options_init` | Usages `0x04` (Keyboard a) to `0xDD` (Keypad Hexadecimal), plus the modifiers. |
| `aoahid_mouse_options_init` | 5 buttons; X and Y -32767..32767 in 16 bits; wheel and pan -127..127 in 8 bits. |
| `aoahid_toggle_options_init` | Consumer Control (`0x0C/0x01`, field page `0x0C`). The Usages and their three parallel arrays are yours to set. |
| `aoahid_gamepad_options_init` | Axes X, Y, Z, Rz (-127..127 in 8 bits, at rest at 0), 12 buttons from Button 1, an eight-way Hat. |
| `aoahid_touchscreen_options_init` | 10 contacts in one report; Contact ID 0..15 in 4 bits; X and Y 0..32767 in 16 bits; Contact Count 0..10 in 4 bits. |
| `aoahid_touchpad_options_init` | As the touchscreen with 5 contacts, plus one click button. |
| `aoahid_pen_options_init`, `aoahid_digitizer_options_init` | X and Y 0..32767 in 16 bits; pressure 0..4095 in 12 bits; one Barrel Switch; hover. |
| `aoahid_raw_options_init`, `aoahid_capability_manifest_init` | `struct_size` only. |

`aoahid_integer_field_make(minimum, maximum, bits)` builds one field without
physical units.

The timeouts are arguments, not options. `AOAHID_CONTROL_TIMEOUT_MS` (500),
`AOAHID_SEND_TIMEOUT_MS` (500) and `AOAHID_DRAIN_TIMEOUT_MS` (1000) are values
that work with the same phones; the library never uses them by itself.

## Handles and ownership

| Handle | Owns | Lifetime |
|---|---|---|
| `aoahid_context` | One libusb context, optionally one event thread, and a graveyard for Devices whose transfers are still draining. | Destroy last. |
| `aoahid_discovery` | A point-in-time device list and every string/pointer returned by `aoahid_discovery_get`. | Independent of the Context once created. |
| `aoahid_device` | One USB handle, a fixed transfer pool, its Nodes and Channels. | Closed by `aoahid_device_close`, which also closes its children. |
| `aoahid_spec` | An immutable descriptor and report layout. Reference-counted (`aoahid_spec_retain` / `aoahid_spec_release`); usable on any number of Devices and Contexts. | Until the last reference is released. |
| `aoahid_node` | Mutable profile state and one AOA HID registration. Retains its Spec. | Until `aoahid_node_close` returns anything but `AOAHID_CLOSE_PENDING`, or its Device closes. |
| `aoahid_channel` | One claimed Bulk interface on the Device's USB handle, its own transfer pool, and read-ahead state. | Until `aoahid_channel_close` returns anything but `AOAHID_CLOSE_PENDING`, or its Device closes. |

AOA HID IDs are allocated per Context and per physical port (bus plus port
path), starting at 1, and are never reused within that Context. Android
tears down a disconnected device's HID objects asynchronously, so reusing an
ID right after a reconnect could collide with an old registration. `aoahid_node_id` returns the ID.

`aoahid_spec_descriptor` and `aoahid_spec_manifest` pointers stay valid while
the Spec reference is held. `aoahid_node_manifest` pointers stay valid until the
Node closes.

## Event modes and threading

`aoahid_context_options.event_mode` is one of two modes
(`aoahid_context_options_init` selects the internal thread):

- `AOAHID_EVENT_CALLER_POLL`: the library creates no thread and takes no
  send-path lock. The application serializes every call on the Context and
  drives completions with `aoahid_context_poll(context, timeout_ms)`
  (`timeout_ms = 0` does not wait). Calling `aoahid_context_poll` in the other
  mode returns `AOAHID_ERR_UNSUPPORTED`.
- `AOAHID_EVENT_INTERNAL_THREAD`: the Context runs one event thread. The
  application must still serialize its own calls on one Context; the thread
  only completes transfers. Channel read and write are the exception described
  under [Bulk Channels](#bulk-channels).

The event thread calls libusb's blocking event handler with a 60-second bound;
any USB event wakes it earlier, so this is not a polling interval. Teardown
sets a stop flag and calls `libusb_interrupt_event_handler`, then joins the
thread. If the backend fails immediately, the thread retains the first failure
for an application thread and waits 10 ms before retrying, so it cannot spin.
See the libusb page
[Multi-threaded applications and asynchronous I/O](https://libusb.sourceforge.io/api-1.0/libusb_mtasync.html).

A Node has at most one report in flight. A state call made while it is in
flight returns `AOAHID_ERR_BUSY`, so a completion never consumes a newer
relative delta or lifecycle edge.

`cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan`
runs the test suite under ThreadSanitizer, including repeated overlap of
completion callbacks and Node close. It covers only the concurrency the API
allows; calls the caller must serialize are still the caller's responsibility.

## Functions

| Group | Functions |
|---|---|
| Diagnostics | `aoahid_last_error`, `aoahid_result_name`, `aoahid_version` |
| Context | `aoahid_context_create`, `aoahid_context_poll`, `aoahid_context_destroy`, `aoahid_context_destroy_blocking` |
| Discovery | `aoahid_discover`, `aoahid_discovery_count`, `aoahid_discovery_get`, `aoahid_discovery_destroy` |
| Accessory mode | `aoahid_accessory_start` |
| Device | `aoahid_device_open`, `aoahid_device_close`, `aoahid_device_latched_error` |
| Channel | `aoahid_channel_open`, `aoahid_channel_close`, `aoahid_channel_write`, `aoahid_channel_read` |
| Spec factories | `aoahid_spec_create_keyboard`, `_mouse`, `_toggle`, `_gamepad`, `_touchscreen`, `_touchpad`, `_pen`, `_digitizer`, `_raw` |
| Spec access | `aoahid_spec_retain`, `aoahid_spec_release`, `aoahid_spec_descriptor`, `aoahid_spec_manifest` |
| Node | `aoahid_node_open`, `aoahid_node_close`, `aoahid_node_id`, `aoahid_node_manifest`, `aoahid_node_submit`, `aoahid_node_submit_blocking` |
| Profile state | `aoahid_keyboard_key`, `aoahid_mouse_move`, `aoahid_mouse_scroll`, `aoahid_mouse_button`, `aoahid_toggle_usage`, `aoahid_gamepad_button`, `aoahid_gamepad_axis`, `aoahid_gamepad_dpad`, `aoahid_touchscreen_contact`, `aoahid_touchpad_contact`, `aoahid_touchpad_button`, `aoahid_pen_update`, `aoahid_digitizer_update`, `aoahid_battery_update`, `aoahid_raw_submit` |

`aoahid_version()` returns `(major << 24) | (minor << 16) | patch`; the header
also defines `AOAHID_VERSION_MAJOR`, `_MINOR`, and `_PATCH`.
`aoahid_result_name()` returns the constant's name, or
`"AOAHID_RESULT_UNKNOWN"` for an unknown value.

## Open sequence

1. `aoahid_context_create`.
2. `aoahid_discover(context, control_timeout_ms, &discovery)` (a zero timeout
   is `AOAHID_ERR_UNSET_FIELD`). It sends AOA request 51 to every USB device and
   lists only those that answer with a nonzero protocol version, reported in
   `aoahid_device_info.protocol_version`. HID needs version 2 or later; the
   library does not enforce that, so check it. To avoid sending request 51,
   skip discovery and fill `aoahid_device_info` from your own enumeration
   (`protocol_version` stays 0 and is ignored by `aoahid_device_open`).
3. `aoahid_device_open`. It opens the device in its current USB mode, sends no
   AOA request, and never waits for re-enumeration. A device without AOA 2.0
   HID support opens normally and then fails at `aoahid_node_open` with
   `AOAHID_ERR_STALL`.
4. Create Specs with the profile factories (no I/O; may run on any thread).
5. `aoahid_node_open` for each Spec. It allocates Node state and any reserved
   pool slot, then synchronously sends request 54 (register) and the
   descriptor in request-56 fragments. A failure unwinds everything it
   allocated.

   **Wait briefly before the first input.** Android's `f_accessory.c` only
   schedules the HID's registration when the last descriptor fragment
   arrives (`schedule_work`), and request 57 is refused until that work has
   added the device. Right after `aoahid_node_open` returns, a report
   therefore STALLs, or is accepted before anything on the phone has opened
   the new input device and is silently lost. On a Galaxy Tab S11, reports
   were refused for 6-16 ms and accepted-but-unseen reports lasted up to
   about 60 ms. Leave a short gap (for example 100 ms) between opening the
   Nodes and sending input.
6. Change state with the profile calls and send it with `aoahid_node_submit`
   or `aoahid_node_submit_blocking`.
7. Optionally `aoahid_channel_open` for Bulk data on the same USB handle, for
   example ADB or the accessory interface of an Android app.

### Switching a device to accessory mode

Switching is not required for HID: `aoahid_device_open` works in the device's
current USB mode, and HID requests 54-57 have been confirmed there on the
devices in [TARGET_MATRIX.md](TARGET_MATRIX.md). The official AOA 2.0 page does
not say whether HID needs `ACCESSORY_START`, so a device that STALLs HID
registration in its current mode can be switched first. The library exposes
the switch as one call and leaves the rest to the application:

1. `aoahid_accessory_start(context, info, &options, control_timeout_ms)` sends
   request 51, request
   52 for each non-null string in `options.strings` (string IDs 0-5), and
   request 53 (START), then closes the device and returns. `manufacturer` and
   `model` are required and must be non-empty; Android matches them against an
   app's accessory filter. Each string may be at most 256 bytes including its
   NUL, checked before any request is sent (`AOAHID_ERR_OVERFLOW`). A request-51
   response shorter than 2 bytes or reporting version 0 returns
   `AOAHID_ERR_NOT_AOA`. A selection already in accessory mode (VID `0x18D1`,
   PID `0x2D00`-`0x2D05`) receives no request.
2. The device disconnects and re-enumerates as VID `0x18D1`, PID `0x2D00`
   (accessory) or `0x2D01` (accessory + ADB); AOA 2.0 audio adds
   `0x2D02`-`0x2D05`. The library does not wait for this or retry. Rediscover
   on your own schedule and match the same bus and port path. Android cancels
   the switch if the host does not configure the device within about 10
   seconds (`ACCESSORY_REQUEST_TIMEOUT` in AOSP
   `frameworks/base/services/usb/java/com/android/server/usb/UsbDeviceManager.java`).
3. Open the new entry with `aoahid_device_open` and continue at step 4 above.

The library has no call that leaves accessory mode. Unplugging the device, or
`adb shell svc usb setFunctions`, returns it to its normal USB functions. If a
Device is still open on the old enumeration, it reports `AOAHID_ERR_NO_DEVICE`
once the device disconnects.

See [PROTOCOL.md](PROTOCOL.md) for the request details.

## Device options

`aoahid_device_options` has three kinds of fields; `aoahid_device_options_init`
fills in all of them.

**Tuning values**, all required and nonzero (see below):
`descriptor_fragment_bytes`, `transfer_pool_slots`, `maximum_report_bytes`.
The waits (control, send, drain) are not options: every call that can wait
takes its timeout as an argument (see "Timeouts" below).

**Size policies**, all required and nonzero:

| Field | Meaning |
|---|---|
| `aoa_descriptor_wire_policy_bytes` | Largest descriptor to send over AOA (at most 65535, the 16-bit AOA length field). |
| `target_ep0_data_policy_bytes` | Largest control-transfer data stage the target accepts. |
| `host_control_buffer_policy_bytes` | Largest control-transfer data stage the host backend accepts. |

`descriptor_fragment_bytes` and `maximum_report_bytes` must be
at most 65535 and no larger than both the EP0 and host-buffer policies, or open
fails with `AOAHID_ERR_OVERFLOW`. The library never enlarges a policy.

**Target HID parser limits**, all required and nonzero, in
`linux_hid` (`aoahid_linux_hid_policy`): `descriptor_bytes`,
`fields_per_report`, `global_stack_depth`, `report_size_bits`, `usages` and
`report_data_bits`. Every Spec records its own requirements
for these while it is built; `aoahid_node_open` compares them with the Device's
policies and returns `AOAHID_ERR_OVERFLOW` before request 54 if one is
exceeded. Pick values for the kernel you target; [LIMITS.md](LIMITS.md) lists
the values in one Linux revision.

**Other fields**:

- `interface_claim_policy`: `AOAHID_INTERFACE_CLAIM_NONE` with
  `interface_number = -1` (HID on EP0 needs no claimed interface), or
  `AOAHID_INTERFACE_CLAIM_EXPLICIT` with `interface_number` 0-255, which is
  claimed on open and released on close.
- `validate_reports`: 0 or 1. With 1, every generated report is decoded again
  before request 57 and checked for wire length, Report ID prefix, field
  ranges, signedness, Null encoding, and zero padding. With 0 the serializer's
  own bounds checks still run. Raw reports are always checked for an exact
  accepted Report ID and length.

`aoahid_device_open` copies the options; your structure is never modified.

## Timeouts

No wait is kept in the library: every call that can wait takes its timeout in
milliseconds as an argument, and zero is refused with `AOAHID_ERR_UNSET_FIELD`
before anything is done (nothing is consumed). libusb
treats a zero timeout as "no timeout", which is why zero is not accepted. A
timeout is a failure deadline, not a delay: a transfer that completes earlier
returns immediately. libmimhid follows the same rule with the same argument
names.

| Argument | Calls | Scope |
|---|---|---|
| `control_timeout_ms` | `aoahid_discover`, `aoahid_accessory_start`, `aoahid_node_open`, `aoahid_node_close`, `aoahid_device_close`, `aoahid_context_destroy`, `aoahid_context_destroy_blocking` | Each synchronous AOA control request (51 to 56). |
| `send_timeout_ms` | `aoahid_node_submit`, `aoahid_raw_submit` | The deadline of the queued asynchronous request-57 transfer; a failure is returned by a later call. A timed-out touch report leaves the contacts it lifted occupied; close and reopen that Node. Keep it well above normal EP0 latency. |
| `drain_timeout_ms` | `aoahid_node_close`, `aoahid_device_close`, `aoahid_context_destroy*`, `aoahid_channel_open`, `aoahid_channel_close` | How long a close (or the wait for in-flight reports before a Channel opens) waits for in-flight reports and for the neutral reports it sends. |
| `timeout_ms` | `aoahid_node_submit_blocking`, `aoahid_channel_read`, `aoahid_channel_write`, `aoahid_context_poll`, `aoahid_context_destroy_blocking` | The whole call. `aoahid_node_submit_blocking` also bounds each report it sends: that report's deadline is what is left of `timeout_ms`; so does the neutral-report wait of a close with `drain_timeout_ms`. |

A Device teardown that cannot finish inside `aoahid_device_close` (a transfer
callback has not run yet) is finished by the Context with the
`control_timeout_ms` of that call.

## Tuning values

Zero in a required one fails with `AOAHID_ERR_UNSET_FIELD`; the initializers
fill all of them in. They are the caller's choices, not USB, AOA, Android, ADB,
or libusb requirements.

| Field | Scope |
|---|---|
| `aoahid_device_options.descriptor_fragment_bytes` | Payload of one request-56 fragment. |
| `aoahid_device_options.transfer_pool_slots` | Asynchronous transfers per Device. |
| `aoahid_device_options.maximum_report_bytes` | Largest report per pool slot. |
| `aoahid_channel_options.in_transfers` | IN transfers kept submitted; required in stream read mode, not used in request mode. |
| `aoahid_channel_options.out_transfers` | OUT transfers; a full pool makes a write wait. |
| `aoahid_channel_options.transfer_bytes` | Bytes per Bulk transfer, rounded up to `wMaxPacketSize`; the largest IN request in request mode. |

Zero keeps its plain meaning in `aoahid_node_options`: `has_reserved_slots = 0`
and `reserved_slots = 0` is no reservation (the Node shares the Device pool).
To reserve a slot for a Node, set `has_reserved_slots = 1` and
`reserved_slots = 1` (a Node has one report in flight, so 0 or 1 is allowed),
leaving at least one shared slot on the Device.

## Bulk Channels

`aoahid_channel_open(device, &options, drain_timeout_ms, &channel)`:

1. waits, within `drain_timeout_ms`, until no HID report
   transfer is in flight (`AOAHID_ERR_TIMEOUT` otherwise). libusb's WinUSB
   backend claims an interface around each control transfer and releases it
   afterwards, which would otherwise undo the Channel's claim;
2. selects configuration 1 only if the device is unconfigured (AOA 1.0 asks the
   host to do so; re-selecting an active configuration would reset the
   device);
3. picks the first interface (alternate setting 0) whose class, subclass, and
   protocol equal `interface_class` / `interface_subclass` /
   `interface_protocol` and that has one Bulk IN and one Bulk OUT endpoint
   (`AOAHID_ERR_UNSUPPORTED` if none), claims it on the Device's handle, and
   allocates the Channel's own transfer pool.

HID and Bulk share one handle, so a composite device never needs a second open
(WinUSB refuses one), and Bulk traffic never uses a HID pool slot. Examples:
ADB is `0xFF/0x42/0x01` (`ADB_CLASS` / `ADB_SUBCLASS` / `ADB_PROTOCOL` in AOSP
`packages/modules/adb/adb.h`); the AOA accessory interface of a
`0x2D00`/`0x2D01` device exists only when manufacturer and model were sent.

Opening returns `AOAHID_ERR_BUSY` when another program holds the interface. For
ADB that is usually a running `adb` server; stop it with `adb kill-server`.
HID on EP0 is unaffected.

### Reading

`read_mode` selects how IN transfers are sized:

| `read_mode` | IN transfers | Use when |
|---|---|---|
| `AOAHID_CHANNEL_READ_STREAM` (1) | `in_transfers` transfers of `transfer_bytes` stay submitted. | The device ends every message with a short or zero-length packet, or `transfer_bytes` is one `wMaxPacketSize`. |
| `AOAHID_CHANNEL_READ_REQUEST` (2) | A read with nothing buffered submits one transfer of its `capacity` rounded up to `wMaxPacketSize` (at most `transfer_bytes`). `in_transfers` is ignored. | The protocol states each length up front, as ADB does. |

A Bulk IN transfer completes only when full or on a short packet. In stream
mode, a message whose length is a multiple of `wMaxPacketSize` and is not
followed by a zero-length packet waits until more data arrives. adbd sends no
such zero-length packet; host adb reads the 24-byte header and then exactly the
payload length. Request mode reads the same way.

`aoahid_channel_read` returns bytes in order and may return part of a
transfer; the caller does its own framing. It returns `AOAHID_OK` with at least
one byte, or `AOAHID_ERR_TIMEOUT` if nothing arrived within `timeout_ms`
(0 never waits). In request mode a timed-out transfer stays pending for the
next call, so no byte is lost; bytes beyond a smaller later `capacity` stay
buffered. Zero-length packets carry no data and are skipped.

### Writing

`aoahid_channel_write` copies data into free OUT transfers and returns once
every byte is submitted, without waiting for completion. If the pool stays full
for `timeout_ms` it returns `AOAHID_ERR_TIMEOUT`; `out_written` always reports
how many bytes were queued. A failed OUT completion is reported once by the
next write. Nothing is retried.

`zero_length_termination = 1` ends a write whose length is a nonzero multiple of
`wMaxPacketSize` with an explicit zero-length transfer, which works on every
backend (libusb's zero-packet flag is Linux-only).

### Errors and threading

A failed IN transfer or a disconnect loses the Channel: reads and writes then
return `AOAHID_ERR_NO_DEVICE`, and the Channel must be closed.

In internal-thread mode one thread may read while another writes, concurrently
with other Context calls, but never concurrently with closing the Channel, its
Device, or the Context. Waiting reads and writes sleep on a condition variable
that the event thread signals. In caller-poll mode read and write belong to the
Context's serialized domain, and a waiting call drives libusb events itself.
Read and write take no lock and do not allocate.

## Submission

`aoahid_node_submit(node, send_timeout_ms)` serializes the Node's pending state into a
pool slot and queues request 57 asynchronously, then returns. In caller-poll mode it may run
one non-waiting poll afterwards. If nothing changed it returns `AOAHID_OK`
without sending.

`aoahid_node_submit_blocking(node, timeout_ms)` keeps submitting and pumping
until the Node has nothing left to send or the timeout expires
(`timeout_ms = 0` is `AOAHID_ERR_UNSET_FIELD`). Use it to flush every
mouse-delta fragment, touch continuation packet, and pen tool-change report in
one call. It returns the completion result for its Node.

Failures of asynchronous completions are returned by the next submit on that
Node and by `aoahid_device_latched_error`. `AOAHID_ERR_NO_DEVICE` is sticky for
the Device; other latched errors are reported once.

The library never retries a transfer:

- `AOAHID_ERR_STALL` on request 57 means the device refused the report, so
  nothing was applied. A common cause is a first report sent before Android
  finished registering the HID device (see step 5 above). The refused state
  stays pending; nothing is resent until you call again, after whatever delay
  you choose:
  - `aoahid_node_submit_blocking` returns the STALL itself; the next submit
    call sends the pending state again.
  - `aoahid_node_submit` returns `AOAHID_OK` once the report is queued. The
    STALL is returned by the next submit call, which sends nothing; the call
    after that sends the pending state again.
  - A Raw Node keeps no copy of its bytes: resend by calling
    `aoahid_raw_submit` again. Until one succeeds, `aoahid_node_submit_blocking`
    on it returns `AOAHID_ERR_UNSUPPORTED`.

  `examples/c/verify/verify_common.h` shows a bounded resend.
- Timeout, cancellation, short transfer, and I/O errors consume the submitted
  state, because the report may have been delivered and resending could repeat
  a key press, contact, or relative motion.

## Profile state calls

Each profile has one set of state calls. Every call validates its arguments
against the Spec, returns `AOAHID_ERR_BUSY` while a report is in flight, and
returns `AOAHID_ERR_NO_DEVICE` once the Device is lost. None of them performs
I/O or allocates.

| Profile | Calls |
|---|---|
| Keyboard | `aoahid_keyboard_key(node, usage, down)` |
| Mouse | `aoahid_mouse_move(node, dx, dy)`, `aoahid_mouse_scroll(node, wheel, pan)`, `aoahid_mouse_button(node, button, down)` |
| Toggle | `aoahid_toggle_usage(node, usage, down)` |
| Gamepad | `aoahid_gamepad_button(node, button, down)`, `aoahid_gamepad_axis(node, axis_index, value)`, `aoahid_gamepad_dpad(node, up, down, right, left)` |
| Touchscreen | `aoahid_touchscreen_contact(node, contact_id, down, x, y, extra)` |
| Touchpad | `aoahid_touchpad_contact(node, contact_id, down, x, y, extra)`, `aoahid_touchpad_button(node, button, down)` |
| Pen | `aoahid_pen_update(node, &sample)` |
| Digitizer | `aoahid_digitizer_update(node, &sample)` |
| Battery report (any profile with `enable_battery`) | `aoahid_battery_update(node, strength)` |
| Raw | `aoahid_raw_submit(node, report, length, send_timeout_ms)` (validates, copies, and queues request 57 directly) |

`down` and the D-pad directions must be exactly 0 or 1. Buttons are
one-based. What each call derives, and which values stay the caller's choice,
is in [PROFILES.md](PROFILES.md). In short:

- **Edge guard.** A press/release edge that has not yet been accepted in a
  report cannot be replaced by the opposite edge; that call returns
  `AOAHID_ERR_BUSY` until the pending edge is sent. This applies to keys,
  buttons, toggles, D-pad, touch contacts, and pen tool state.
- **Keyboard.** `usage` is a modifier (`0xE0`-`0xE7`) or a Usage in the Spec's
  range. There is no release-all call; closing the Node sends a report with
  every key up.
- **Mouse.** Deltas accumulate in signed 64-bit totals (overflow returns
  `AOAHID_ERR_OVERFLOW`); `aoahid_mouse_scroll` on an axis the Spec did not
  enable returns `AOAHID_ERR_UNSUPPORTED`.
- **Toggle.** `down = 1` presses an allow-listed `usage` and `down = 0` releases
  that same `usage`. Each Usage is its own bit, so several can be held at once.
- **Touch.** `down = 1` places a new `contact_id` or moves an active one;
  `down = 0` lifts an active one at the given final x/y. `extra` may be null;
  a nonzero value for a field the Spec did not enable is rejected. No free
  slot returns `AOAHID_ERR_OVERFLOW`.
- **Pen and Digitizer.** Each call sends the whole state as one sample. There is
  no separate depart call: `in_range = 0` takes the tool away, and X/Y, tilt,
  and twist in that sample go out as given, so repeat the last position to
  leave the pointer where it was.
- **Touch together with a mouse.** A Touchscreen Node and a Mouse Node on the
  same device are separate input devices. Observed on a Galaxy Tab S11: when
  both report at the same time, Android drops the one that was there first,
  so a mouse report sent while a touch contact is down cancels the contact
  (the contact does not end as a lift). libaoahid does not prevent this and
  has no primary source for it; hold the mouse reports back while a contact is
  down, and send the contact's lift before the next mouse report.
- **Battery report.** `aoahid_battery_update` works on a Node whose Spec was
  created with `enable_battery = 1`. The level is sent by the next submit that
  has no input report to send, and only when it changed.

No profile generates key repeat, debounce, long-press, or any other timed
action.

### C++ header

`include/aoahid.hpp` forwards every function of `aoahid.h` unchanged under
namespace `aoahid`, without the `aoahid_` prefix (`aoahid_keyboard_key` is
`aoahid::keyboard_key`). Types, arguments, and results are those of the C API.
It adds two things:

- one function per initializer that returns the initialized structure
  (`aoahid::device_options` returns what `aoahid_device_options_init` fills in);
- `aoahid::SpecPtr` and `aoahid::DiscoveryPtr`, `std::unique_ptr` types that
  release a Spec reference or destroy a discovery snapshot. Context, Device,
  Node and Channel stay plain pointers: closing them takes timeouts and may
  return `AOAHID_CLOSE_PENDING`, which a destructor could neither report nor
  retry.

## Manifest

`aoahid_spec_manifest` and `aoahid_node_manifest` fill an
`aoahid_capability_manifest` (set `struct_size`, zero `reserved`):

- `input_supported = 1`, `output_supported = 0`,
  `feature_transport_supported = 0`;
- `android_status`: `AOAHID_ANDROID_PORTABLE_CANDIDATE` for a form that follows
  documented Android input behavior, `AOAHID_ANDROID_CONDITIONAL` when behavior
  depends on the Android release, kernel, or OEM, and `AOAHID_ANDROID_UNKNOWN`
  for Raw Specs. This is a static classification of the descriptor, not a test
  result; see [PROFILES.md](PROFILES.md#hardware-status) for what has been
  tested;
- `profile_kind`;
- `reports` / `report_count`: every Input Report ID and its exact wire length;
- `descriptor_length`.

A generated descriptor may contain a Constant Feature item such as Contact
Count Maximum. It is metadata only; `feature_transport_supported` stays 0 and
there is no Feature or Output path back to the host.

## Close and destroy

Closing a Node waits for its in-flight report, sends a neutral/release report
if the device is still present, then sends request 55 (unregister). No
transfer, buffer, handle, or object is freed before its libusb callback has
run.

Every close and destroy call follows one rule:

| Result | The handle |
|---|---|
| `AOAHID_ERR_PARAM`, `AOAHID_ERR_UNSET_FIELD` | Nothing was done; it is still yours. |
| `AOAHID_CLOSE_PENDING` | A transfer of the object is still in flight; it is still yours. Let events run (`aoahid_context_poll` in caller-poll mode) and call again. |
| Anything else | The object is gone. An error says what failed on the way out, such as a report that failed earlier or a refused unregister (request 55). |

Where `AOAHID_CLOSE_PENDING` can come from:

- `aoahid_node_close`: a report of the Node is still in flight at the drain
  deadline.
- `aoahid_channel_close`: a cancelled Bulk transfer has not completed.
- `aoahid_context_destroy`: a Device teardown is still waiting for a callback.
- `aoahid_context_destroy_blocking`: as `aoahid_context_destroy`, when its own
  `timeout_ms` passed first.

`aoahid_device_close` never returns it. When a transfer of the Device is still
in flight at the drain deadline, the Context takes the teardown over, frees
everything once the callbacks have run, and reports any error of it from
`aoahid_context_poll` or `aoahid_context_destroy`. A consumed pointer must never
be passed to the API again.

## Errors

`aoahid_last_error()` returns the calling thread's diagnostic record without
changing it. Every other function replaces the record: success resets it to
`AOAHID_OK`; a failure fills `code`, `field`, `reason`, and, where they apply,
`transport_status` (the libusb status), `aoa_request`, `node_id` (the AOA HID ID of the Node involved), `report_id`, `offset`, and `length`.
The strings stay valid until the thread's next call into the library.
Functions that return a value rather than a result (`aoahid_discovery_count`,
`aoahid_discovery_get`, `aoahid_node_id`, `aoahid_version`) use 0 or null as
the failure value, and `void` functions report failure only through
`aoahid_last_error()`.

Output handles (`out_context`, `out_discovery`, `out_device`, `out_spec`,
`out_node`, `out_channel`) are null on every failure.

The event thread never writes to an application thread's record. Completion
errors are stored on the Node, Device, or Context and published on the next
application call that touches them. If the event thread itself stops, its
error is returned by every later call on that Context. A submit that libusb
accepted stays successful even if the poll that follows it fails; that poll
error is returned by the next call.

| Code | Meaning |
|---|---|
| `AOAHID_ERR_PARAM` | Invalid pointer, handle, struct size, or argument. |
| `AOAHID_ERR_UNSET_FIELD` | A required field or timeout is zero, or a field has no valid value. |
| `AOAHID_ERR_UNSUPPORTED` | Not supported by this profile, mode, or backend. |
| `AOAHID_ERR_NOT_AOA` | The device does not report AOA support (`aoahid_accessory_start`). |
| `AOAHID_ERR_ACCESS`, `_BUSY`, `_NO_DEVICE`, `_TIMEOUT`, `_IO` | Mapped libusb status. `AOAHID_ERR_BUSY` also means "report in flight" for state calls. `AOAHID_ERR_NO_DEVICE` is sticky for the Device. |
| `AOAHID_ERR_STALL` | A control request stalled. This alone does not prove the descriptor was rejected. |
| `AOAHID_ERR_SHORT_TRANSFER` | A control transfer moved fewer bytes than its payload (the 8-byte setup packet is not counted). |
| `AOAHID_ERR_DESCRIPTOR_REJECTED` | Reserved result code; `aoahid_result_name` knows it. |
| `AOAHID_ERR_OVERFLOW` | A size, count, or policy limit was exceeded, or libusb reported `LIBUSB_TRANSFER_OVERFLOW` (the device sent more data than requested). |
| `AOAHID_CLOSE_PENDING` | A close could not finish within its budget; the handle is still the caller's. |
| `AOAHID_ERR_INTERNAL` | Allocation failure or unexpected internal error. |

No C++ exception crosses the C ABI. Allocation failures and unexpected
exceptions are caught at the exported function, returned as
`AOAHID_ERR_INTERNAL`, and partly built objects release their USB and HID
resources first.

## Logging

`aoahid_context_options.log_level` is `AOAHID_LOG_DISABLED`, `_ERROR`, `_INFO`,
or `_TRACE`. Any level other than disabled requires a non-null `log_sink`
(`AOAHID_ERR_UNSET_FIELD` otherwise). Only cold paths (open, close,
registration, errors) log; submission and completion never do. Records include
bus/port path, VID/PID, HID and Report IDs, request, offset, length, result,
and libusb status. Report payload bytes are never logged.

The sink is called synchronously on the thread that produced the record,
including the internal event thread. It must return promptly, must not call
back into or destroy the same Context, and must be thread-safe in
internal-thread mode. The library holds no internal lock while calling it.
