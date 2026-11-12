# Limits

This page separates four kinds of limits that are easy to confuse: rules from
the HID specification, values in one Linux kernel revision, limits this library
chooses, and transport tuning values. A value from one layer is never presented
as a rule of another. Profile behavior is in [PROFILES.md](PROFILES.md); option
fields are in [API.md](API.md).

## HID 1.11 rules

Source: [Device Class Definition for HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf)
and [HID Usage Tables 1.7](https://usb.org/sites/default/files/hut1_7.pdf).

| Subject | Rule | Section | What the library does |
|---|---|---|---|
| Short-item data | 0, 1, 2, or 4 bytes | §6.2.2.2 | Never writes a 3-byte item. |
| Report ID | 1-255; 0 is reserved | §6.2.2.7 | An enabled Report ID must be 1-255. Without a Report ID item, reports have no prefix byte. |
| Report identity | Report type plus Report ID | §§6.2.2.7, 8 | Input, Output, and Feature layouts are tracked separately; generated profiles are Input only. |
| Report ID placement | Before the first Main item of the report it identifies | §6.2.2.7 | An Application Collection may come first. |
| Field span | One field touches at most 4 bytes; a 32-bit field starts on a byte boundary | §8.4 | Checked per field, not per report. |
| Report length | No 4-byte limit on the whole report | §8.4 | Report size is limited only by the transport and target policies below. |
| Signed items | A field is signed when either extent is negative, unsigned otherwise | §§5.8, 6.2.2.7 | Minimum items are written as the shortest signed value. A Maximum is written as the shortest unsigned value when both extents are non-negative (65535 with minimum 0 is `26 FF FF`), and as the shortest signed value otherwise. |
| Unit Exponent | 4-bit signed, -8..7 | §6.2.2.7 | Emitted as a low nibble. |
| Global tags | 0-11 defined, 12-15 reserved | §6.2.2.7 | Raw validation rejects reserved tags. |
| Extended Usages | A 4-byte Usage carries its own page; an extended Usage Minimum needs an extended Maximum | §6.2.2.8 | Raw validation rejects mixed pairs. |
| Units | A non-None Unit needs Logical and Physical extents and a Unit Exponent | HUT §3.3 | Raw validation tracks these through Push/Pop. |
| Null State | Needs at least one encodable value outside the logical range | §6.2.2.5 | A Hat field whose range fills its width is rejected. |

### Field span

For a field at bit offset `o` with width `w`:

```text
bytes_touched = ceil(((o mod 8) + w) / 8)
```

The field is valid when `bytes_touched <= 4`, and when `w == 32` also
`o mod 8 == 0`.

| Example | Bytes touched | Result |
|---|---:|---|
| 32 bits at offset 0 | 4 | Accepted |
| 32 bits at offset 1 | 5 | Rejected |
| 31 bits at offset 1 | 4 | Accepted |
| Six 8-bit fields | 1 each, 6-byte report | Accepted |

### Field ordering inside generated profiles

A field's offset depends on the fields before it, so a 1-4-bit group (buttons,
Hat, D-pad bits, Tip Switch) can push a following wide field across a fifth
byte. The generator therefore pads to a byte boundary between caller-sized
fields:

- Mouse: after the buttons, after X/Y, after Wheel, after Pan.
- Gamepad: after the buttons, after the Hat or D-pad bits, and after every
  axis.
- Pen: after the tool and barrel bits, after X/Y, after Pressure, after
  Tilt X/Y, after Twist.
- Touchscreen and Touchpad: in every contact after Tip Switch, after the
  Contact Identifier/X/Y group, and after each of Pressure, Width, and Height;
  then before Scan Time and before Contact Count.

Two caller-sized fields can also follow each other with no boundary between
them (Mouse X then Y, Pen X then Y, a contact's Contact Identifier, X, and Y).
There the generator pads to the next byte boundary only when the second field
would otherwise touch a fifth byte, for example a 32-bit Y behind a 31-bit X;
narrower fields stay packed.

This costs at most a few padding bits per boundary and keeps every
combination of widths within the HID field-span rule.

### Value ranges

For width `w` (1-32):

```text
unsigned: 0 .. 2^w - 1
signed:   -2^(w-1) .. 2^(w-1) - 1
```

The logical range must fit the width. The width is always explicit; it is not
derived from the range.

## Linux HID values

These are values in one kernel revision, Android common kernel commit
`35556bed836f8dc07ac55f69c8d17dce3e7f0e25` (2020-09-01). Other kernels may
differ; check the same symbols in the kernel you target.

| Symbol or function | Value or behavior | File | Consequence |
|---|---:|---|---|
| `HID_MAX_IDS` | 256 | `include/linux/hid.h` | Report IDs 0-255 per report type; 0 means "no ID". |
| `HID_MAX_FIELDS` | 256 | `include/linux/hid.h`, `hid_register_field` | Fields per report (per type and Report ID), not per descriptor. |
| `HID_MAX_USAGES` | 12288 | `include/linux/hid.h`, `hid_parser_global` | Upper bound on Report Count and stored Local Usages. |
| `HID_MAX_BUFFER_SIZE` | 8192 bytes | `include/linux/hid.h` | A kernel buffer size; not an AOA descriptor limit or an EP0 size. |
| `HID_GLOBAL_STACK_SIZE` | 4 | `include/linux/hid.h`, `hid_parser_global` | A fifth nested Push is rejected. |
| Report Size | at most 256 bits per declaration | `hid_parser_global` | Checked when declared, even if later overwritten. |
| Report data | at most 65,528 bits per report (`(HID_MAX_BUFFER_SIZE - 1) << 3`) | `hid_add_field` | Excludes the Report ID byte. |
| Long items | Rejected (any prefix with tag 15) | `fetch_item`, `hid_open_report` | Raw validation checks the tag nibble, not only `0xFE`. |
| Usage ranges | The current page is added only when the Maximum item is 1 or 2 bytes | `hid_parser_local`, `hid_add_usage` | The library counts Usage demand exactly as this code does. |
| Standalone Usage Maximum | Expands from 0 because Local state starts zeroed | `hid_parser_local` | Accepted by this kernel; HID 1.11 does not define it. |
| Input fields wider than 32 bits | Warned about and truncated to 32 | `hid_field_extract` | Generated fields are at most 32 bits. |
| `MT_DEFAULT_MAXCONTACT` | 10 | `drivers/hid/hid-multitouch.c` | Used when nothing else gives a contact maximum. |
| `MT_MAX_MAXCONTACT` | 250 | `drivers/hid/hid-multitouch.c` | Upper bound for a Contact Count Maximum Feature value. |
| `mt_compute_timestamp` | Scan Time delta × 100 | `drivers/hid/hid-multitouch.c` | Scan Time must be in 100 µs units. |
| `hidinput_setup_battery` | Only with `CONFIG_HID_BATTERY_STRENGTH` | `drivers/hid/hid-input.c` | Without it, Battery Strength is ignored. |
| `hidinput_update_battery` (v5.10 to v6.12) | Ignores raw 0 and out-of-range values for every battery usage | `drivers/hid/hid-input.c` | A level of 0 is never published when the range starts at 0; start the range at 1. |
| `hidinput_update_battery` (later) | Ignores out-of-range values, and raw 0 only on the Digitizers page | `drivers/hid/hid-input.c` | Out-of-range values never produce an update, so there is no "unknown" to send. |
| `hidinput_setup_battery` (through v6.12) | One battery per HID device | `drivers/hid/hid-input.c` | A Spec has at most one battery report. |
| `hidinput_connect` | A HID device with no input usages gets no input device, and its power supply is unregistered | `drivers/hid/hid-input.c` | The battery report rides on an input profile; there is no battery-only profile. |

The `linux_hid` fields of `aoahid_device_options` (`descriptor_bytes`,
`fields_per_report`, `usages`, `global_stack_depth`, `report_size_bits`,
`report_data_bits`) correspond to the first rows of this table. For the revision
above they are 4096, 256, 12288, 4, 256 and 65528, the values
`aoahid_device_options_init` fills in; check them against your target kernel. Each Spec records what it needs,
using checked arithmetic without expanding Usage ranges, and `aoahid_node_open`
returns `AOAHID_ERR_OVERFLOW` before registering if a policy is exceeded.

## Library limits

These are choices made by this library, not HID, Linux, or Android limits.

| Subject | Limit |
|---|---|
| Generated field width | 1-32 bits. |
| Touch contacts | `maximum_contacts` 1-16 (Touchscreen and Touchpad). |
| Contacts per report | 1..`maximum_contacts`; fewer only with `enable_multi_packet_frames`. |
| Buttons | Mouse 1-65535, Gamepad at least 1 (Usage range must stay within 16 bits), Touchpad 0-65535. |
| Gamepad axes | At least 2 (one X, one Y), each role once, at most 65535. |
| Toggle allow-list | 1-65535 unique nonzero Usages. |
| Pen barrel controls | At most 32, each `0x44` or `0x5A`, unique. |
| Pressure (touch, pen) | Range must include 0 and at least 1. |
| Canonical Hat | Logical 0-7, 4 bits, Null sent as 15. |
| Mouse deltas | Signed 64-bit pending total per axis; overflow is an error. |
| Toggle semantics | Selector, OOC toggle, OOC maintained, MC, OSC, RTC only. |
| Raw report table | At most 256 entries; exact ID and wire length (a `uint32_t`). |
| Raw descriptors | Short items only; reserved Main, Local, and item-type values rejected; a 4-byte Usage Maximum of `0xFFFFFFFF` rejected; every Collection has a Usage; every non-Constant Input has Usage Page, Usage, Logical Minimum/Maximum, Report Size, and Report Count; Delimiter sets close before their Main item. |
| AOA descriptor | At most 65535 bytes (16-bit AOA length field), further limited by `aoa_descriptor_wire_policy_bytes` and `linux_hid.descriptor_bytes`. |
| Accessory strings | At most 256 bytes each, including the NUL. |
| Object counts | No fixed maximum for Contexts, Devices, Nodes, or Specs; allocation can still fail. |

## Transport tuning

Descriptor size, report size, EP0 size, and host buffer size are separate
policies (see [API.md](API.md#device-options)) and are not derived from any
value above. The tuning values below are required (zero is
`AOAHID_ERR_UNSET_FIELD`) and are the caller's choices, not USB, Android, or
libusb recommendations; see [API.md](API.md#tuning-values).

`descriptor_fragment_bytes`, `transfer_pool_slots`, `maximum_report_bytes`, and
the Channel's `out_transfers`, `transfer_bytes` and (stream mode)
`in_transfers`; the initializers fill them in. The waits (`control_timeout_ms`,
`send_timeout_ms`, `drain_timeout_ms`) are arguments of the calls that wait; see
[API.md](API.md#timeouts).
`aoahid_node_options` `0/0` is no reserved slot.

`maximum_report_bytes` and `descriptor_fragment_bytes` must fit
`target_ep0_data_policy_bytes` and `host_control_buffer_policy_bytes`.

The Touchscreen, Keyboard, Mouse, Gamepad, and Consumer
Control profiles have been used on the devices listed in
[PROFILES.md](PROFILES.md#hardware-status). For latency figures see
[LATENCY.md](LATENCY.md).

## Changing a limit

A change should name the specification section or kernel symbol and revision
it relies on, state whether it affects descriptor encoding, serialization,
transport allocation, or only validation, and add tests just below, at, and
just above the new value.
