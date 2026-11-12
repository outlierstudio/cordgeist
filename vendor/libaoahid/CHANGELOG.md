# Changelog

All notable changes to libaoahid are recorded here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.3] - 2026-11-02

Not hardware-verified: the library, its ABI and its behavior are those of 2.0.2.
The changes are in the build files, the examples, the tests and the tooling; the
release archives carry the new `examples/CMakeLists.txt` and `examples/cpp/`. The
build layout is now the one libmimhid has.

### Added

- `AOAHID_WERROR`, on by default, which keeps the warnings-as-errors the build always had and lets a build with a newer compiler turn it off.
- `examples/CMakeLists.txt`: the examples build on their own against an installed package (`find_package(aoahid)`) as well as from the source tree; the targets and CTest names are those of before.
- `examples/cpp/multitouch_gesture.cpp`: a C++ example of `aoahid.hpp`, built as `aoahid_example_cpp`.
- `tools/check_input_provenance.py` also fails when the README, CONTRIBUTING or the docs name an `AOAHID_*` build option (in a `-D` argument or, for option-shaped names, in code quotes) that the CMake files do not declare; it used to check only the spelling of the prefix.

### Fixed

- `docs/EXAMPLES.md` said the verification programs are seven; they are six.

## [2.0.2] - 2026-11-02

Not hardware-verified: the library, its ABI and its behavior are those of 2.0.1.
The changes are in the language bindings, the examples and the tooling, and are
tested against the fake libusb backend and the compiled C ABI.

### Changed

- The Python, C#, and Rust declarations and `tests/abi/layout_oracle.c` are generated from `include/aoahid.h` by `tools/bindings/generate.py`; `tools/check_bindings.py` is gone, replaced by `generate.py --check`. Public names are unchanged. Additions: the three `AOAHID_*_TIMEOUT_MS` constants in the layout oracle and the bindings, the C# `nint` forms of the functions that take a structure pointer, and in C# the `AbiStructs` dictionary and `fixed` buffers for array fields. The C# `AbiConstants` values are `long`. `sync_version.py` now runs the generator.

### Removed

- The Python, C# and Rust `multi_profile` examples (`examples/python/multi_profile.py`, `examples/csharp/MultiProfile/`, `examples/rust/`, together about 2,200 lines) and the CI steps that ran them. The life cycle with reconnection and threads is shown once, in `examples/c/multi_profile.c`; each other language keeps its short enumerate example. The bindings keep their public names.

## [2.0.1] - 2026-10-30

Not hardware-verified: only comments in `aoahid.h` change. The library, its ABI
and its behavior are those of 2.0.0.

### Fixed

- `aoahid.h` says that `AOAHID_ERR_DESCRIPTOR_REJECTED` is reserved: no call
  returns it. It keeps number 12 because libmimhid returns its own code of that
  number when a board refuses a configuration.
- A line break in the `Returns` of `aoahid_context_destroy_blocking`.

## [2.0.0] - 2026-10-29

Not hardware-verified on a phone: this release changes the API around the
transport and the profiles, not how they work, and those stay as
hardware-verified in 1.0.0. The changes are tested against the fake libusb
backend (including a ThreadSanitizer build) and the bindings against the
compiled C ABI.

The ABI changes so that libaoahid and libmimhid follow the same rules for waits,
closing and initial values, and use the same names for the same things.

### Changed (breaking)

- **One ownership rule for every close and destroy.** `AOAHID_ERR_PARAM` or
  `AOAHID_ERR_UNSET_FIELD`: nothing was done. `AOAHID_CLOSE_PENDING`: a transfer
  is still in flight and the handle is still the caller's; call again. Any
  other result: the object is gone, and an error says what failed on the way
  out. Before, a failed `aoahid_node_close` or `aoahid_channel_close` left the
  handle with the caller.
  - `aoahid_node_close` runs every step (finish the touch frame, neutral report,
    request 55) even when one fails, and returns the first failure.
  - `aoahid_device_close` never returns `AOAHID_CLOSE_PENDING`: when a transfer
    is still in flight, the Context finishes the teardown and reports its errors
    from `aoahid_context_poll` or `aoahid_context_destroy`.
  - `aoahid_context_destroy_blocking` returns `AOAHID_CLOSE_PENDING` (it returned
    `AOAHID_ERR_TIMEOUT`) when its own deadline passes first.
- **Every timeout is an argument of the call; none is set at open.** The three
  `aoahid_device_options` fields `control_timeout_ms`, `send_timeout_ms` and
  `close_drain_timeout_ms` and `aoahid_accessory_options.control_timeout_ms` are
  removed. A zero timeout is refused with `AOAHID_ERR_UNSET_FIELD` before
  anything is done, in `aoahid_discover` and the deadline of
  `aoahid_context_destroy_blocking` too (both returned `AOAHID_ERR_PARAM`). New
  signatures:
  - `aoahid_accessory_start(context, selected, options, control_timeout_ms)`
  - `aoahid_node_open(device, spec, options, control_timeout_ms, &node)`
  - `aoahid_node_close(node, control_timeout_ms, drain_timeout_ms)`
  - `aoahid_node_submit(node, send_timeout_ms)`
  - `aoahid_raw_submit(node, report, length, send_timeout_ms)`
  - `aoahid_device_close(device, control_timeout_ms, drain_timeout_ms)`
  - `aoahid_context_destroy(context, control_timeout_ms, drain_timeout_ms)`
  - `aoahid_context_destroy_blocking(context, control_timeout_ms, drain_timeout_ms, timeout_ms)`
  - `aoahid_channel_open(device, options, drain_timeout_ms, &channel)`
  - `aoahid_channel_close(channel, drain_timeout_ms)`

  `aoahid_node_submit_blocking` keeps its `timeout_ms`; it now also bounds each
  report it sends (that report's deadline is what is left of it), and a close
  bounds the neutral reports it sends by what is left of `drain_timeout_ms`.
- **The Linux HID parser limits are one structure,** `aoahid_linux_hid_policy`,
  in `aoahid_device_options.linux_hid`: `descriptor_bytes`, `fields_per_report`,
  `global_stack_depth`, `report_size_bits`, `usages` and `report_data_bits`
  replace `linux_descriptor_policy_bytes` and the five `linux_hid_*_policy`
  fields.
- **`AOAHID_CHANNEL_READ_STREAM` is 1 and `AOAHID_CHANNEL_READ_REQUEST` is 2.**
  Every enumeration now starts at 1, so a `read_mode` left zero is refused
  instead of selecting stream mode.
- **`aoahid_node_hid_id` is `aoahid_node_id`**, as in libmimhid (C#: `Native.NodeId`).
- **`aoahid_error_detail` fields `libusb_status` and `hid_id` are `transport_status`
  and `node_id`**, as in libmimhid, and `code` is typed `aoahid_result` (the same
  `int32_t`). The log keys follow (`transport_status=`, `node_id=`).
- **The shared library's SONAME is `libaoahid.so.2`** (`libaoahid.2.dylib`) and its
  ELF version node is `AOAHID_2.0`, so a program built against 1.x fails to load
  against this library instead of calling functions with the old arguments.

### Added

- **Initializers.** `aoahid_<structure>_init` for every options structure fills
  in values that work with a current Android phone, so a caller changes only
  what it needs; `aoahid_integer_field_make` builds one field.
  `AOAHID_CONTROL_TIMEOUT_MS`, `AOAHID_SEND_TIMEOUT_MS` and
  `AOAHID_DRAIN_TIMEOUT_MS` are timeouts that work with the same phones. They
  are `static inline` in the header, so the binary ABI has nothing new; the
  library still never puts a value of its own in place of a zero. The values
  are listed in "Initial values" in `docs/API.md`.
- `aoahid.hpp` returns initialized structures (`aoahid::device_options`, ...)
  and has `aoahid::SpecPtr` and `aoahid::DiscoveryPtr`, `std::unique_ptr` types
  that release a Spec or a discovery snapshot.

### Fixed

- The examples drop a Node handle after every close result but
  `AOAHID_CLOSE_PENDING`, as the new rule requires.
- The profile examples counted buttons from 0 in their printed call sequences;
  buttons count from 1.

### Migration

- Pass the waits you gave in `aoahid_device_options` and
  `aoahid_accessory_options` to the calls above, for example
  `aoahid_node_submit(node, 500U)` where `send_timeout_ms` was 500.
- Move the Linux policy values into `options.linux_hid`, or start from
  `aoahid_device_options_init`, which fills in the values of `docs/LIMITS.md`.
- Set `read_mode` explicitly; zero is no longer stream mode.
- Treat a Node or Channel as gone after any close result but
  `AOAHID_CLOSE_PENDING`, and stop expecting `AOAHID_CLOSE_PENDING` from
  `aoahid_device_close`.

The C++ forwarders, the Python, C# and Rust bindings and the examples follow
the same signatures.

## [1.0.1] - 2026-10-18

Not hardware-verified on a phone: this release changes how options are checked,
not the transport or the profiles, which stay as hardware-verified in 1.0.0.
The changes are tested against the fake libusb backend.

### Changed

- **No tuning defaults.** A zero no longer selects a library value. The Device's
  `control_timeout_ms`, `send_timeout_ms`, `descriptor_fragment_bytes`,
  `transfer_pool_slots`, `maximum_report_bytes` and `close_drain_timeout_ms`,
  the accessory's `control_timeout_ms`, and the Channel's `out_transfers`,
  `transfer_bytes` and (stream read mode) `in_transfers` are required; zero is
  `AOAHID_ERR_UNSET_FIELD`. Callers that set every value, as the examples do,
  are unaffected. Node reservation `0/0` still means no reservation, and
  `read_mode` 0 is still `AOAHID_CHANNEL_READ_STREAM`.

### Added

- `AOAHID_CHECK_SERIALIZATION` build option (off by default): the library ends
  the process, naming the second call, when two threads are inside calls on one
  Context. A debugging aid for the caller-serialization rule; with it off the
  built library is unchanged. See `docs/ARCHITECTURE.md`.

## [1.0.0] - 2026-10-11

The first stable release. Hardware-verified on a Samsung Galaxy Tab S11
(Arch Linux):
Keyboard, Mouse, Toggle (media keys), Gamepad, Touchscreen, Touchpad, Pen,
and Digitizer were verified via devtest and kernel getevent. A battery report
on a Keyboard was registered there too (the kernel creates the power supply
and Android classes the device `BATTERY`); the reported level was not read
back.


### Changed (breaking)

- A battery level is now a second Input report inside an input profile, not a
  profile of its own. Every generated profile's options end with
  `enable_battery` and `battery` (`aoahid_battery_report`: `report_id`,
  `strength`); when enabled, the profile's Application Collection carries a
  Battery Strength (`0x06/0x20`) report under its own Report ID. Linux drops a
  HID device that has no input usages, so the standalone form never produced a
  power supply.
- `aoahid_spec_create_battery`, `aoahid_battery_options`, and
  `AOAHID_PROFILE_BATTERY` are removed.
- `aoahid_battery_update(node, strength)` takes the level only and works on
  any Node whose Spec has a battery report. There is no "unknown" value:
  Linux drops out-of-range battery values. The report is sent by a submit that
  has no input report to send, and only when the level changed.
- Error fields name the structure the caller filled in: Touchscreen option
  errors are `touchscreen.*` (they were `touch.*`), Digitizer option and
  sample errors are `digitizer.*` (they were `pen.*`), and the contact errors
  shared by Touchscreen and Touchpad carry their own profile's name.
- The profile numbers are reordered: `KEYBOARD` 1, `MOUSE` 2, `TOUCHSCREEN` 3,
  `TOUCHPAD` 4, `PEN` 5, `DIGITIZER` 6, `GAMEPAD` 7, `TOGGLE` 8, `RAW` 9.
- The Pen profile is split in two, like Touch Screen and Touch Pad. `PEN` is
  the direct-screen stylus under Digitizers / Pen (`0x0D/0x02`). The indirect
  tablet form is the new `AOAHID_PROFILE_DIGITIZER` (= 6), created with
  `aoahid_spec_create_digitizer(&aoahid_digitizer_options)`, under Digitizers /
  Digitizer (`0x0D/0x01`).
- `aoahid_pen_options` no longer has a `mode` field. `aoahid_pen_mode`,
  `AOAHID_PEN_DIRECT_SCREEN`, and `AOAHID_PEN_INDIRECT_TABLET` are removed.
- The descriptors are byte-identical to the former `mode` forms; the golden
  file `pen-indirect.hex` is now `digitizer.hex`.
- Every profile that Android recognizes differently has its own send calls and
  structures. `aoahid_touch` is replaced by `aoahid_touchscreen_contact` and
  `aoahid_touchpad_contact`, and `aoahid_touch_extra` by `aoahid_touchscreen_extra`
  and `aoahid_touchpad_extra`. `aoahid_pen_update` takes
  Pen Nodes only; Digitizer Nodes use `aoahid_digitizer_update` and
  `aoahid_digitizer_sample`. Leaving range is a sample with `in_range = 0`;
  there is no separate depart call. Each call rejects a
  Node of the other profile with `AOAHID_ERR_PARAM`. The C++ forwarders and the
  three bindings follow.
- The Keyboard and Gamepad calls take the profile's name too: `aoahid_kbd` is
  `aoahid_keyboard_key` and `aoahid_dpad` is `aoahid_gamepad_dpad` (C++
  `aoahid::keyboard_key` and `aoahid::gamepad_dpad`; C# `KeyboardKey` and
  `GamepadDpad`). Every send call is now `aoahid_<profile>_<operation>`.
- The Toggle call is `aoahid_toggle_usage` (C++ `aoahid::toggle_usage`, C#
  `ToggleUsage`), so every send call follows `aoahid_<profile>_<operation>`. It
  works like `aoahid_keyboard_key`: any number of allowed Usages can be pressed
  at once, each is released by its own Usage, and a Usage that is not in the
  allow-list (including 0) is `AOAHID_ERR_PARAM`. It used to hold one Usage at
  a time, and 0 released whichever Usage was pressed.
- `aoahid_touch_contact` is no longer public: no public function takes it. The
  library keeps it internally, and the Python, C#, and Rust bindings and the
  examples no longer declare it.
- Names that did not follow the rest of the API are aligned, with no change in
  behavior: the gamepad axis call is `aoahid_gamepad_axis` (it was
  `aoahid_gamepad_set_axis`) and its Spec entry type is
  `aoahid_gamepad_axis_option` (it was `aoahid_gamepad_axis`, which the call
  could not share), like `aoahid_report_id_option`; the touch options field is
  `contact_id` (it was `contact_identifier`), as in the send calls;
  `aoahid_capability_manifest.descriptor_bytes` is `descriptor_length`, as in
  `aoahid_raw_options`; the two Linux parser limits in bits are
  `linux_hid_report_data_policy_bits` and `linux_hid_report_size_policy_bits`,
  with the unit last like the other `*_policy_bytes` fields. The Python, C#,
  and Rust bindings follow (C# `GamepadAxis`, `GamepadAxisOption`,
  `ContactId`, `DescriptorLength`, `LinuxHidReportDataPolicyBits`,
  `LinuxHidReportSizePolicyBits`).
- A Report ID has one shape everywhere: `aoahid_raw_report` and
  `aoahid_report_capability` hold an `aoahid_report_id_option report_id` (its
  `enabled` and `value`) and `wire_length`, like the options of every profile.
  They had `report_id`, `has_report_id`, and `reserved16`. The two structures
  are 12 bytes now, not 8. The raw report table and the options share one
  check of the ID (`aoa::hid::report_id_option_violation`); a table entry with
  a nonzero reserved byte is `AOAHID_ERR_PARAM` as before, and the unit tests
  now cover each rule.
- The button calls name their flag `down`, like the other send calls, and
  `aoahid_node_submit_blocking` names its wait `timeout_ms` (it was
  `deadline_ms`). Only parameter names changed.

### Added

- `aoahid_digitizer_options`, `aoahid_spec_create_digitizer`,
  `aoahid::spec_create_digitizer`, `AOAHID_PROFILE_DIGITIZER`, and the matching
  Python, C#, and Rust binding entries; the example
  `examples/c/profiles/digitizer.c`.

### Changed

- On Linux and other non-Apple Unix targets the shared library links the C++
  runtime statically (`-static-libstdc++ -static-libgcc`) and exports no
  runtime symbols, so it no longer depends on `libstdc++` or `libgcc_s`.
