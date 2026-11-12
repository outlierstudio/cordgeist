# Architecture

This document describes how libaoahid is put together. It is for contributors
and for users who need to reason about threading, lifetimes, or failure
handling. For the wire protocol see [PROTOCOL.md](PROTOCOL.md); for the full
function reference see [API.md](API.md); for the implementation behind it
(locks, the send path, teardown) see [INTERNALS.md](INTERNALS.md).

## Layers

```
application
   │  C ABI (include/aoahid.h)  or  C++ header (include/aoahid.hpp, namespace aoahid)
   ▼
src/api        ABI boundary: argument/struct validation, handle lifetimes, error records
src/profiles   immutable Spec per profile + per-Node report state machines
src/hid        HID report descriptor writer, builder, validator
src/transport  libusb: discovery, AOA control requests, report transfer pool, bulk Channels
   │
   ▼
libusb 1.0 (runtime 1.0.30 or later is required)
```

No libusb or C++ type crosses the public header. `src/transport/transport.hpp`
is the internal boundary: the layers above it see only plain host-side values.

## Object model

| Object | Created by | Owns / borrows |
| --- | --- | --- |
| Context | `aoahid_context_create` | The libusb context, the event mode, the optional event thread, and a graveyard for teardown that could not finish in time. |
| Discovery | `aoahid_discover` | A snapshot of devices that answered AOA request 51 with a nonzero version. Independent lifetime. |
| Device | `aoahid_device_open` | One opened USB device in its current mode, a fixed transfer pool, and optionally one claimed interface. |
| Spec | `aoahid_spec_create_*` | An immutable, reference-counted profile description and its generated HID report descriptor. Not tied to a Context. |
| Node | `aoahid_node_open(device, spec, ...)` | One AOA HID ID registered on the Device for one Spec, plus that Node's report state. Retains its Spec. |
| Channel | `aoahid_channel_open` | A bulk IN/OUT interface on the same USB handle as the Device (used, for example, for ADB). |

Relationships: Context → Device → { Node…, Channel… }. Closing a Device closes
every Node and Channel still open on it; destroying a Context tears down every
child graph.

Internally a USB handle is held by a reference-counted `Port`. A Device and its
Channels borrow the same Port, and discovery on an already open device probes
through that Port instead of opening a second handle (a second open fails under
WinUSB). Interface claims are counted per interface.

### HID IDs

AOA HID IDs are chosen by the host. The library allocates them from a
monotonic counter starting at 1, kept per physical identity (USB bus plus port
path) within a Context, so IDs are not reused while the Context lives, even if
the device re-enumerates. Exhausting the 16-bit space returns
`AOAHID_ERR_OVERFLOW`. `aoahid_node_id` returns the ID of a Node.

## Event modes and threading

The mode is fixed at `aoahid_context_create`:

| Mode | Who drives libusb events | Locking |
| --- | --- | --- |
| `AOAHID_EVENT_CALLER_POLL` | The caller, through `aoahid_context_poll`. Transfer callbacks run inside that call. No library thread is created (the platform libusb backend may still own helper threads). | No internal send-path lock. |
| `AOAHID_EVENT_INTERNAL_THREAD` | One library event thread. | Pool and error-latch access is synchronized, which adds contention that caller-poll mode avoids. |

Serialization rules, from the header contracts:

- Every application call that touches one Context (including its Devices,
  Nodes, and Channels) must be serialized by the caller. The library does not
  make concurrent application calls on one Context safe, in either mode.
- Separate Contexts are independent.
- Spec objects are immutable; concurrent reads are allowed while each reader
  holds a reference (`aoahid_spec_retain` / `aoahid_spec_release`).
- `aoahid_last_error` is thread-local and valid until that thread's next call.
- Never call close/destroy functions from the log sink.

### Checking the serialization rule

Breaking the first rule is undefined behavior that usually shows up far from
its cause. A library built with `-DAOAHID_CHECK_SERIALIZATION=ON` (off by
default; on in the `dev` preset) ends the process, with a line on stderr that
names the second call (the function, or for the report-state calls their
field name, such as `mouse.move`), when a thread enters a call on a Context
while another thread is inside one. It is a debugging aid, not a lock: it adds
an atomic exchange to every checked call and is not meant for release builds,
whose code it does not change when off. It is left out of the `tsan` preset:
its own atomic orders the calls of different threads, which would hide from
ThreadSanitizer the unsynchronized calls that happen not to overlap.

It checks the calls whose contract in `aoahid.h` says they must be serialized
with every other call on the Context. It does not check:

- `aoahid_channel_read` and `aoahid_channel_write` in internal-thread mode
  (one reader and one writer may run beside other calls there), or a Channel
  read racing its own close;
- `aoahid_node_id` and `aoahid_node_manifest`, which only have to be kept
  apart from the Node's teardown;
- the Spec functions and the discovery snapshot readers;
- a call on a null or closed handle;
- a call made from inside another call on the same thread (the log sink must
  not call into its Context, and this does not catch it);
- a call that enters after `aoahid_context_destroy` or
  `aoahid_context_destroy_blocking` has started: those two only look, on
  entry, for a thread already inside.

## Send path

Each Node has at most one report in flight.

1. **Update.** A profile call (`aoahid_keyboard_key`, `aoahid_touchscreen_contact`, `aoahid_mouse_move`,
   `aoahid_gamepad_axis`, …) changes the Node's state. It does not block,
   allocate, log, or do I/O. It returns `AOAHID_ERR_BUSY` while a report (or a
   multi-packet touch frame) is in flight.
2. **Submit.** `aoahid_node_submit` serializes the dirty state directly into a
   slot of the Device's transfer pool and submits it as an asynchronous AOA
   request 57 control transfer. It does not wait for completion; in caller-poll
   mode it may run one zero-timeout event pump after acceptance.
   `aoahid_node_submit_blocking` additionally waits, up to an explicit timeout,
   until every dirty report has completed. `aoahid_raw_submit` copies caller
   bytes for a Raw Node.
3. **Completion.** The libusb callback (in `aoahid_context_poll` or on the event
   thread) returns the slot to the pool. A failure is latched on the Device and
   reported by the next call or by `aoahid_device_latched_error`.

With `validate_reports = 1` in `aoahid_device_options`, each generated report
is decoded again against the Spec before request 57 is sent.

### Transfer pool

`aoahid_device_open` allocates a fixed number of slots
(`transfer_pool_slots`). Each slot holds one libusb transfer and a buffer of
the 8-byte setup packet plus `maximum_report_bytes`. The setup fields that never change are written once; per
submission only the HID ID (wValue) and length (wLength) are stored. No
allocation happens on the send path.

The pool is shared by every Node on the Device. A Node can reserve one slot
(`aoahid_node_options.has_reserved_slots` / `reserved_slots`, zero or one) so
other Nodes cannot starve it; reservations must always leave at least one
shared slot. When no usable slot is free, submission returns `AOAHID_ERR_BUSY`.

### Registration and close

`aoahid_node_open` synchronously sends request 54 (register) and the
descriptor in request 56 fragments of `descriptor_fragment_bytes`. If registration fails after the device may have accepted request 54, a
best-effort request 55 is sent so no half-registered ID is left behind.

`aoahid_node_close` waits up to its `drain_timeout_ms` argument (zero is
`AOAHID_ERR_UNSET_FIELD`) for in-flight work, finishes any touch frame, sends the neutral
reports the profile requires (every key and button up, contacts lifted, axes at
rest), and then sends request 55. A failed step does not stop the next one. If
a report is still in flight at the deadline, the Node close returns
`AOAHID_CLOSE_PENDING` and the Node stays the caller's; a Device close instead
moves the consumed graph to the Context graveyard, which reaps it once the
callbacks have run.

## Design rules

- **No inferred product or target values.** Usages, logical ranges, bit
  widths, Report IDs, and target HID-parser limits are caller inputs. The
  library does not guess them. An unset required value is an error
  (`AOAHID_ERR_UNSET_FIELD` or `AOAHID_ERR_PARAM`), never a silent fallback.
- **Initial values in the header, none in the library.** The `*_init`
  functions in `aoahid.h` fill options in; the library itself never puts a
  value in place of a zero. Timeouts, fragment size, pool size, maximum report
  size, drain budget, and Channel transfer counts and size are required; zero
  is `AOAHID_ERR_UNSET_FIELD`. Every enumeration starts at 1, so a zeroed
  enumeration field is refused too. Options are copied; caller storage is never
  modified. Reserved fields must be zero.
- **No convenience names.** `tools/symbol-lint/symbol_lint.py` fails if an
  exported function name contains `default`, `preset`, `standard`, or
  `typical` as an underscore-separated word. It also checks that the header
  does not include libusb, that every `*_options` struct (and
  `aoahid_capability_manifest`) begins with `uint32_t struct_size`, and that
  the built library's exports match the header.

## Error model

- Every fallible function returns `aoahid_result` (`int32_t`). `AOAHID_OK` is
  0; errors are the `AOAHID_ERR_*` constants; `AOAHID_CLOSE_PENDING` means a
  close could not finish within its budget and the handle is still the
  caller's. Every close and destroy follows the same ownership rule (see
  "Closing" at the top of the header).
- `aoahid_last_error()` returns a thread-local `aoahid_error_detail`: code,
  the field name, a reason string, the raw libusb status, the AOA request
  number, HID ID, Report ID, and offset/length where relevant.
- `aoahid_result_name()` maps a code to its name.
- Completion errors are latched per Device. `AOAHID_ERR_NO_DEVICE` is sticky;
  other latched errors are consumed when reported.
- C++ exceptions never cross the ABI. Allocation failure and unexpected
  exceptions become `AOAHID_ERR_INTERNAL`.

## ABI stability

- All public enums are `int32_t` typedefs with anonymous-enum constants, so
  struct layout does not depend on compiler enum sizing.
- Every options struct starts with `uint32_t struct_size`, which must equal
  `sizeof` the struct in the release the caller links against; a mismatch
  returns `AOAHID_ERR_PARAM`. Most also carry a `reserved` field that must be
  zero.
- Handles are opaque pointers. The `*_init` functions are `static inline` in
  the header, so they add nothing to the binary ABI.
- `AOAHID_VERSION_MAJOR/MINOR/PATCH` and `aoahid_version()` report the version.
- Layout is guarded by `tests/abi` (C and C++ ABI tests, a layout oracle, and
  `check_abi_golden.py`) and by the generated bindings: the Python, C#, and
  Rust declarations come from `aoahid.h` through `tools/bindings/generate.py`,
  and each language compares its layout with the oracle.
- The library is built with `-fvisibility=hidden`; only `AOAHID_API` symbols
  are exported.

## Source tree

| Path | Contents |
| --- | --- |
| `include/aoahid.h` | The stable C ABI, with per-function ownership, blocking, synchronization, and result contracts. |
| `include/aoahid.hpp` | Header-only C++ forwarders to the C ABI (namespace `aoahid`). |
| `src/api/` | `c_api.cpp` (ABI boundary, lifetimes, HID ID allocation, event thread), `error_detail.cpp` (thread-local error record, struct validation). |
| `src/hid/` | Item writer, descriptor builder, descriptor validator, report layout. |
| `src/profiles/` | `spec.cpp` (Spec factories and descriptor generation), `state.cpp` (per-Node report state machines and serialization). |
| `src/transport/` | `discovery.cpp` (runtime, event pumping, request-51 probing), `port.cpp` (shared USB handle, accessory start), `transport.cpp` (requests 54–57, transfer pool), `channel.cpp` (bulk Channels). |
| `bindings/` | Python, C#, and Rust bindings over the C ABI. |
| `tests/` | `unit/`, `integration/` (against the fake libusb in `fake_libusb/`), `abi/`, `golden/` (expected descriptors as hex), `fuzz/`, `cmake/` (package tests), `hidtools/`, `release/`. |
| `tools/` | `symbol-lint/`, `check_*.py` contract and license checks, `generate-support-table/`, `device-check/`, `release/`, `docs/`. |

## Building and checking

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
./build/dev/aoahid_tests profiles   # only the named suites: item_writer, profiles, transport, identity
```

Tests require `AOAHID_USE_FAKE_LIBUSB=ON`; the fake backend is never used in
release builds. `-DAOAHID_SANITIZE=ON` enables ASan and UBSan. Library code
compiles with `-Werror -Wconversion -Wsign-conversion -fno-rtti`. CI also runs
clang-format, clang-tidy, the symbol lint, and the `tools/check_*.py` scripts.

## Hardware status

Which devices, hosts, and profiles have been confirmed on real hardware is
recorded in [TARGET_MATRIX.md](TARGET_MATRIX.md); the profiles are described in
[PROFILES.md](PROFILES.md).
