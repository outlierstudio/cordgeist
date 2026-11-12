# HID Input Profiles

libaoahid builds HID report descriptors for nine profile kinds and keeps a
state machine per Node that turns simple calls (`aoahid_keyboard_key`, `aoahid_touchscreen_contact`,
...) into reports. This page describes each descriptor, what the library
derives for you, and which values you must choose. The C calls are summarized
in [API.md](API.md); numeric limits are in [LIMITS.md](LIMITS.md).

Usage IDs are from the
[HID Usage Tables 1.7](https://usb.org/sites/default/files/hut1_7.pdf) (HUT);
report rules are from
[Device Class Definition for HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf)
(HID 1.11).

## Hardware status

Tested on real hardware (Samsung Galaxy Tab S11 and POCO F6 Pro, from Windows
10 x64 and Arch Linux hosts):

- Touchscreen
- Touchpad
- Keyboard
- Mouse
- Gamepad
- Toggle as Consumer Control media keys
- Pen
- Digitizer

Not yet tested on hardware: Raw, and Toggle on the System Control,
Camera Control, and Telephony pages. See [TARGET_MATRIX.md](TARGET_MATRIX.md)
for per-target results.

The manifest's `android_status` is a static classification of the descriptor
form, independent of this testing:

| Profile | `android_status` |
|---|---|
| Mouse, Touchscreen, Pen | `AOAHID_ANDROID_PORTABLE_CANDIDATE` |
| Gamepad with canonical Hat and Buttons starting at 1 with at least 5 buttons | `AOAHID_ANDROID_PORTABLE_CANDIDATE` |
| Keyboard, Toggle, Touchpad, Digitizer, other Gamepad forms | `AOAHID_ANDROID_CONDITIONAL` |
| Raw | `AOAHID_ANDROID_UNKNOWN` |

"Portable candidate" means the form follows documented Android input behavior;
"conditional" means the result depends on the Android release, kernel
configuration, key layout, or OEM.

## Rules shared by every profile

1. **Input only.** Profiles emit Input reports only. There is no Output report
   (for example keyboard LEDs or rumble) and no Feature response. A descriptor
   may contain Constant Feature metadata such as Contact Count Maximum; it does
   not add a Feature transport.
2. **Report ID.** When `report_id.enabled = 1`, `report_id.value` is 1-255
   (HID 1.11 §6.2.2.7 reserves 0), the Report ID item follows the Application
   Collection and precedes the first Input item, and every report starts with
   that byte. When disabled, no prefix byte is sent.
3. **One Application Collection** per descriptor; a report never spans two
   top-level collections (HID 1.11 §8.4).
4. **Field encoding.** Values are little-endian at the offset fixed by the
   descriptor; signed fields use two's complement. Every generated field has an
   explicit width of 1-32 bits and its logical range must fit that width. No
   field touches more than four bytes, and a 32-bit field starts on a byte
   boundary (HID 1.11 §8.4). The generator inserts Constant padding to a byte
   boundary between caller-sized fields so this holds for every width
   combination (see [LIMITS.md](LIMITS.md#field-ordering-inside-generated-profiles)).
   Trailing bits are zero padding.
5. **Physical properties.** Every `aoahid_integer_field` has an optional
   `physical` block (Physical Minimum/Maximum, Unit Exponent, Unit). When
   enabled, the four Global items are emitted for that field and reset right
   after it.
6. **Neutral on close.** Closing a Node sends its release/neutral state before
   request 55, if the device is still present.
7. **Edge guard.** A press/release (or down/up, tool) edge stays pending until
   a report carrying it has been accepted. The opposite edge is rejected with
   `AOAHID_ERR_BUSY` until then, so a quick tap is never lost by coalescing.
8. **No timers.** No profile generates key repeat, debounce, long-press,
   re-trigger cadence, or any other timed event.

The library never infers a product value: Usages, Application Collection,
Report ID, logical and physical ranges, widths, axis neutrals, contact counts,
button counts, and optional-field presence are what the options say. The
`*_init` functions fill in one working set of them
([API.md](API.md#initial-values)); a factory reads only what the structure
holds.

## Keyboard

`aoahid_spec_create_keyboard(&aoahid_keyboard_options)`

- Application Collection: Generic Desktop / Keyboard (`0x01/0x06`).
- Eight modifier bits, Keyboard page `0x07` Usages `0xE0`-`0xE7` (Left
  Control, Left Shift, Left Alt, Left GUI, Right Control, Right Shift, Right
  Alt, Right GUI).
- One bit per Usage from `usage_minimum` to `usage_maximum` (full N-key
  rollover). `usage_minimum` must be at least `0x04` (0x00-0x03 are error
  codes) and the range must not overlap `0xE0`-`0xE7`.
- No Array field, so no slot count and no ErrorRollOver encoding. HUT §3.4.2.1
  allows Selectors as Variable bits.
- No LED Output report.

`aoahid_keyboard_key(node, usage, down)` routes a modifier to its bit and any other
Usage to its bitmap bit; a Usage outside the declared range is
`AOAHID_ERR_PARAM`. Repeating a press or release that is already in effect
changes nothing. There is no release-all call; release every key you pressed
before closing, or the target may keep it held (close does clear all bits).

Typed text also depends on the target's key layout, character map, locale, and
IME; a barcode-wedge scanner uses the same profile.

## Mouse

`aoahid_spec_create_mouse(&aoahid_mouse_options)`

- Application Collection: Generic Desktop / Mouse (`0x01/0x02`), with a
  Pointer (`0x01`) Physical Collection.
- `button_count` buttons (1-65535), Button page Usages 1..n.
- X (`0x30`) and Y (`0x31`), Relative. Their ranges must contain negative and
  positive values.
- Optional Wheel (Generic Desktop `0x38`, `enable_wheel`) and horizontal pan
  (Consumer AC Pan `0x0C/0x0238`, `enable_pan`), Relative, with ranges that
  contain negative and positive values.

`aoahid_mouse_move` and `aoahid_mouse_scroll` add to signed 64-bit pending
totals for X, Y, Wheel, and Pan (overflow is `AOAHID_ERR_OVERFLOW`). Each report
clamps every total to its field range and sends that fragment with the current
buttons; the rest stays pending for the next report. A completed report
consumes only what it carried, so a timeout never causes an automatic duplicate
move. `aoahid_node_submit_blocking` sends fragments until all totals are zero.
Pointer acceleration is the target's business.

## Toggle: Consumer, System, Camera, and Telephony controls

`aoahid_spec_create_toggle(&aoahid_toggle_options)`

One factory covers every page of one-bit controls. You choose:

- `application_page` / `application_usage`: the Application Collection;
- `field_page`: the Usage Page of every entry in `allowed_usages`;
- `allowed_usages[i]`, `usage_semantics[i]`, `expected_linux_event_types[i]`,
  and `expected_linux_codes[i]` (four parallel arrays of `allowed_usage_count`
  entries; all required, Usages nonzero and unique).

The expected Linux event strings are stored with the Spec as documentation of
what you expect the target kernel to produce (for example `EV_KEY` /
`KEY_PLAYPAUSE`); the library does not interpret them.

Common choices:

| Controls | `application_page`/`application_usage` | `field_page` |
|---|---|---|
| Consumer Control (media keys) | `0x0C`/`0x01` | `0x0C` |
| System Control (power, sleep) | `0x01`/`0x80` | `0x01` |
| Camera Control | `0x0C`/`0x01` | `0x90` |
| Telephony | `0x0B`/`0x01` (Phone) | `0x0B` |

Each allow-listed Usage is its own one-bit Variable field, so sparse Usage IDs
never declare the IDs between them. The flags come from the semantic you
select (HUT §3.4.1 and Table 3.2); the library does not guess a semantic from
the Usage number:

| `usage_semantics[i]` | Input flags |
|---|---|
| `AOAHID_USAGE_SELECTOR_BITMAP` | Variable, Absolute, Preferred State |
| `AOAHID_USAGE_ON_OFF_TOGGLE` (OOC, single button) | Variable, Relative, Preferred State |
| `AOAHID_USAGE_ON_OFF_MAINTAINED` (OOC, maintained switch) | Variable, Absolute, No Preferred |
| `AOAHID_USAGE_MOMENTARY` (MC) | Variable, Absolute, Preferred State |
| `AOAHID_USAGE_ONE_SHOT` (OSC) | Variable, Relative, Preferred State |
| `AOAHID_USAGE_RETRIGGER` (RTC) | Variable, Absolute, Preferred State |

`AOAHID_USAGE_ON_OFF_PAIR`, `_LINEAR`, `_DYNAMIC_VALUE`, and `_NAMED_ARRAY` need
a value, direction, or collection that press/release cannot express, so the
factory returns `AOAHID_ERR_UNSUPPORTED`. For example, Consumer Volume
(`0x00E0`) is a Linear Control and cannot be used here; use Volume Increment
(`0x00E9`) and Volume Decrement (`0x00EA`) instead.

With `field_page = 0x90`, only Auto-focus (`0x20`, else `AOAHID_ERR_PARAM`) and
Shutter (`0x21`) are allowed, each as `AOAHID_USAGE_ONE_SHOT` (else
`AOAHID_ERR_UNSUPPORTED`); HUT 1.7 defines no other Camera Control Usage.

`aoahid_toggle_usage(node, usage, 1)` sets the bit of one allow-listed Usage and
`aoahid_toggle_usage(node, usage, 0)` clears it; a Usage outside the allow-list is
`AOAHID_ERR_PARAM`. Any number of controls can be active at once, and they go
out in one report. There is no release-all call; release every Usage you
pressed before closing the Node (close does clear all bits). A tap is
`down = 1`, submit, `down = 0`, submit; the edge guard keeps the press from
being lost even if you submit only once in between.

Whether the target delivers a control to apps, or the system intercepts it,
depends on Android's key layout and policy.

## Gamepad

`aoahid_spec_create_gamepad(&aoahid_gamepad_options)`

- Application Collection: Generic Desktop / Game Pad (`0x01/0x05`).
- Buttons: `button_count` (at least 1) Button page Usages starting at
  `button_usage_minimum` (nonzero).
- D-pad (`dpad_representation`):
  - `AOAHID_DPAD_HAT`: one Hat Switch (`0x39`). You must set
    `hat_logical_minimum = 0`, `hat_logical_maximum = 7`, `hat_bit_width = 4`;
    the library adds Physical 0-315, Unit Degrees (`0x14`), Unit Exponent 0,
    and the Null State flag.
  - `AOAHID_DPAD_BUTTONS`: four one-bit fields D-pad Up/Down/Right/Left
    (`0x90`-`0x93`).
  - `AOAHID_DPAD_NONE`: no direction field.

  Outside Hat mode the three `hat_*` fields must be zero.
- Axes: `axis_count` (at least 2, including exactly one `AOAHID_AXIS_X` and one
  `AOAHID_AXIS_Y`) entries of `aoahid_gamepad_axis_option`. Each has a role, the
  matching Usage Page and Usage, a value field with its own width, an in-range
  `neutral_value`, and non-empty `expected_linux_code` /
  `expected_android_axis` strings (stored with the Spec, not interpreted).
  Each role may appear once.

| Role | Page / Usage |
|---|---|
| X, Y, Z, Rx, Ry, Rz | Generic Desktop `0x30`-`0x35` |
| Slider, Dial, Wheel | Generic Desktop `0x36`, `0x37`, `0x38` |
| Rudder, Throttle | Simulation Controls `0x02` / `0xBA`, `0xBB` |
| Accelerator, Brake, Steering | Simulation Controls `0x02` / `0xC4`, `0xC5`, `0xC8` |

Accelerator, Brake, and Throttle require logical minimum 0, a positive maximum,
and neutral 0. Steering and Rudder require a range from negative to positive
and neutral 0. For other axes the neutral is whatever you declare; the library
never computes a midpoint or decides that Z/Rz is a trigger.

`aoahid_gamepad_dpad(node, up, down, right, left)` works in Hat and Buttons modes
(`AOAHID_ERR_PARAM` in None mode). In Hat mode:

| Direction | Up | Down | Right | Left | Hat value |
|---|---:|---:|---:|---:|---:|
| none | 0 | 0 | 0 | 0 | 15 (Null) |
| Up | 1 | 0 | 0 | 0 | 0 |
| Up-right | 1 | 0 | 1 | 0 | 1 |
| Right | 0 | 0 | 1 | 0 | 2 |
| Down-right | 0 | 1 | 1 | 0 | 3 |
| Down | 0 | 1 | 0 | 0 | 4 |
| Down-left | 0 | 1 | 0 | 1 | 5 |
| Left | 0 | 0 | 0 | 1 | 6 |
| Up-left | 1 | 0 | 0 | 1 | 7 |

Up+Down or Left+Right returns `AOAHID_ERR_PARAM` and leaves the state
unchanged. For "no direction" any value outside 0-7 is a valid HID Null
(HID 1.11 §6.2.2.5); the library always sends 15. In Buttons mode the four bits
are sent as given, including opposite pairs.

The Hat form matches the
[Android CDD §7.2.6.1](https://source.android.com/docs/compatibility/17/android-17-cdd)
game controller mapping (Hat logical 0-7, physical 0-315 degrees, report size
4; A, B, X, Y on Button Usages 1, 2, 4, 5). That is why a Gamepad is a
portable candidate only with the Hat and a Button range starting at 1 with at
least 5 buttons.

`aoahid_gamepad_axis(node, axis_index, value)` takes the index into your
`axes` array. On close, buttons clear, the Hat sends Null, D-pad bits clear,
and each axis returns to its neutral.

## Touchscreen

`aoahid_spec_create_touchscreen(&aoahid_touchscreen_options)`

- Application Collection: Digitizers / Touch Screen (`0x0D/0x04`), fixed-slot
  Multi-Touch.
- `contacts_per_report` Finger (`0x22`) Logical Collections, each with Tip
  Switch (`0x42`), Contact Identifier (`0x51`), X/Y (Generic Desktop
  `0x30`/`0x31`), and optional Tip Pressure (`0x30`), Width (`0x48`), Height
  (`0x49`), Azimuth (`0x3F`).
- After the contacts: optional Scan Time (`0x56`) and Contact Count (`0x54`).
- Optional Contact Count Maximum (`0x55`) as a Constant Feature item
  (`enable_contact_count_maximum_feature_declaration`).

Constraints (all `AOAHID_ERR_PARAM`):

- `maximum_contacts` 1-16; `contacts_per_report` 1..`maximum_contacts`, and
  equal to it unless `enable_multi_packet_frames = 1`.
- X and Y ranges include 0.
- Contact Identifier range includes 0 and at least `maximum_contacts` values.
- Contact Count range covers 0..`maximum_contacts`.
- Pressure: minimum 0, maximum at least 1.
- Width and Height ranges include 0 (inactive slots are zero-filled); Width
  must use the same Unit and Unit Exponent as X, Height the same as Y.
- Azimuth: minimum 0, positive maximum.
- Scan Time: `scan_time_unit_100us = 1`, minimum 0, maximum at least 1, no
  physical block.

### Contacts

`aoahid_touchscreen_contact(node, contact_id, down, x, y, extra)` for a Touchscreen
Node and `aoahid_touchpad_contact(node, contact_id, down, x, y, extra)` for a
Touchpad Node (each rejects the other profile's Node):

- `down = 1` with an inactive `contact_id` places a new contact (Tip = 1);
  with an active one it moves it.
- `down = 0` lifts an active contact at the given final position: the next
  report carries that contact with Tip = 0 and the same Contact Identifier.
- A `contact_id` cannot be placed again until its lift report has completed.

The library always sends an explicit Tip = 0 record; it never relies on a
contact disappearing from the report, because the default Linux
`hid-multitouch` class does not treat an unseen contact as lifted.

When pressure is enabled, a touching contact's pressure is sent as at least 1;
a lifted contact sends 0.

### Contact Count and multi-packet frames

Contact Count is the number of contact records in the frame, including Tip = 0
records. With more active contacts than `contacts_per_report`, the frame is
split: the first report carries the total count, continuation reports carry 0,
and unused slots are zero-filled. This is how Linux `hid-multitouch`
(`mt_touch_report`) reassembles a frame. State calls return `AOAHID_ERR_BUSY`
until the whole frame has been sent. After the last contact is lifted, no
empty report is sent.

### Scan Time

When enabled, Scan Time is filled in on the first report of each frame as the
time since the first frame after inactivity, in 100 µs units, wrapped at
`logical_maximum + 1`; continuation reports repeat it. HUT §16 defines Scan
Time as a relative time in 100 µs units; Linux `hid-multitouch` assumes that
unit. The count restarts at 0 once every contact is lifted and sent.

## Touchpad

`aoahid_spec_create_touchpad(&aoahid_touchpad_options)`

The same fixed-slot Multi-Touch descriptor and contact state machine as
Touchscreen, with every field and constraint identical, except:

- Application Collection: Digitizers / Touch Pad (`0x0D/0x05`);
- `button_count` click buttons (0-65535; 0 for a buttonless clickpad), Button
  page Usages 1..n after Contact Count;
- `android_status` is always `AOAHID_ANDROID_CONDITIONAL`. Android turns
  touchpad input into mouse-style pointer motion, and gesture support varies
  by release and OEM.

`aoahid_touchpad_contact` takes the same arguments as `aoahid_touchscreen_contact`,
with `aoahid_touchpad_extra`. `aoahid_touchpad_button(node, button,
pressed)` sets a one-based button; it is rejected with `AOAHID_ERR_BUSY` while
a multi-packet contact frame is being sent. With `button_count = 0` every
button index is out of range. Close clears buttons and lifts all contacts.

## Pen and Digitizer

`aoahid_spec_create_pen(&aoahid_pen_options)` and
`aoahid_spec_create_digitizer(&aoahid_digitizer_options)`

The two profiles share one report shape and one state machine; the options
structures have the same fields. They differ only in the Application
Collection Usage and in how Linux and Android classify the device:

| | Pen | Digitizer |
|---|---|---|
| Application Collection | Digitizers / Pen (`0x0D/0x02`) | Digitizers / Digitizer (`0x0D/0x01`) |
| Linux input property | `INPUT_PROP_DIRECT` | `INPUT_PROP_POINTER` |
| `android_status` | `AOAHID_ANDROID_PORTABLE_CANDIDATE` | `AOAHID_ANDROID_CONDITIONAL` |

Both use a Stylus (`0x20`) Physical Collection. A Pen Node is driven with
`aoahid_pen_update` (`aoahid_pen_sample`), a Digitizer Node with
`aoahid_digitizer_update` (`aoahid_digitizer_sample`, the same fields); each rejects the other profile's
Node.

- Bits, in this order: Invert (`0x3C`, only with `enable_eraser`), Tip Switch
  (`0x42`), In Range (`0x32`), then one bit per barrel Usage.
- `barrel_usages`: up to 32 unique entries, each Barrel Switch (`0x44`) or
  Secondary Barrel Switch (`0x5A`).
- X/Y (ranges include 0), then optional Tip Pressure (`0x30`, minimum 0,
  maximum at least 1), X Tilt / Y Tilt (`0x3D`/`0x3E`, enabled together,
  ranges include 0), and Twist (`0x41`, range includes 0).

The eraser end is signaled with Invert, not with the separate Eraser Usage
(`0x45`), because Linux maps both Tip Switch and Eraser to `BTN_TOUCH`. Invert
comes before Tip Switch and In Range because Linux reads the Invert state when
In Range turns on to choose the pen or rubber tool.

`aoahid_pen_update(node, &sample)` and `aoahid_digitizer_update(node, &sample)`:

- `tip = 1` requires `in_range = 1`. Without `enable_hover`, `in_range` must
  equal `tip`.
- `eraser = 1` requires `enable_eraser`. `barrel_buttons` is a bitmask over
  `barrel_usages`.
- Disabled optional fields must be 0.
- A touching pen's pressure is sent as at least 1.
- Switching between pen and eraser while in range first sends an
  out-of-range report, then re-enters with the new tool.

A sample with `in_range = 0`, or close, sends In Range, Tip, pressure, Invert,
and barrel bits as 0. In a sample, X/Y, tilt, and twist go out as given: repeat
the last position to leave the pointer where it was. The eraser selection is remembered for the next entry.

| State | In Range | Tip | Pressure |
|---|---:|---:|---:|
| Away | 0 | 0 | 0 |
| Hover | 1 | 0 | 0 |
| Contact | 1 | 1 | at least 1 (if enabled) |

Display association, rotation, and tilt/twist handling depend on the target.

## Battery report

Every generated profile (Keyboard, Mouse, Touchscreen, Touchpad, Pen,
Digitizer, Gamepad, Toggle) takes `enable_battery` and `battery`
(`aoahid_battery_report`) at the end of its options.

- `enable_battery = 0`: nothing changes; `battery` must be all zero.
- `enable_battery = 1`: the profile's Application Collection gets a second
  Input report with one field, Battery Strength (`0x06/0x20`). The profile's
  `report_id` and `battery.report_id` must both be enabled and differ; the
  library does not pick them. The range is yours (minimum at least 0, maximum
  greater than minimum, fitting `bit_width`).

There is no battery device on its own. Linux registers a HID battery only
with an input device: `hidinput_connect()` drops a HID device that has no
input usages and unregisters the power supply it had set up (see
[LIMITS.md](LIMITS.md#linux-hid-values)).

`aoahid_battery_update(node, strength)` sets the level. The report goes out on
the next `aoahid_node_submit` that has no input report to send, so input is
never held back to send a battery level, and it is sent only when the level
changed. A Node has one report in flight: while the battery report is on the
wire, an input update returns `AOAHID_ERR_BUSY` like any other. Closing a Node
does not send a level that was still waiting.

There is no "unknown" to send. Linux drops any battery value outside the
declared range and reports the level as unknown until the first value arrives,
so a Node that was never updated sends nothing and the target shows no level.

Use a range that starts at 1 (for example 1 to 100). Linux v5.10 through
v6.12 ignores a raw value of 0 for every battery usage; later kernels ignore
it only on the Digitizers page. Linux reports
`(value - minimum) * 100 / (maximum - minimum)` percent. How often to send is
yours: Linux forwards an unchanged level at most once in 30 seconds, and every
forwarded change is a `power_supply` uevent.

Linux reports Battery Strength through its power-supply class only when the
kernel is built with `CONFIG_HID_BATTERY_STRENGTH`. On Android an app reads
the level with `InputDevice.getBatteryState()`.

## Raw

`aoahid_spec_create_raw(&aoahid_raw_options)` accepts your own descriptor plus a
table of accepted Input reports (`aoahid_raw_report`: a `report_id`
(`aoahid_report_id_option`: whether it is present, and the ID) and the exact
`wire_length`). You must set
`acknowledges_no_android_support = 1` and `requires_output =
requires_feature_response = 0` (else `AOAHID_ERR_UNSUPPORTED`).

The descriptor is parsed and checked (short items only, no reserved items,
required Global/Local state present, field spans per HID 1.11 §8.4, see
[LIMITS.md](LIMITS.md#library-limits)). `aoahid_raw_submit(node, report,
length)` checks the report against the accepted ID and length (and that
trailing padding bits are zero), copies it, and queues it. No state, neutral
report, or transition is generated; `aoahid_node_submit` does not apply to Raw
Nodes.

## Checking a profile on a target

To confirm a profile on a device, record on the same device and build:

1. that the kernel parsed the descriptor and which `/dev/input/event*`
   capabilities it created;
2. `adb shell dumpsys input`: classification, sources, and ranges;
3. `adb shell getevent -lt` for press/release or down/move/up sequences;
4. the `KeyEvent` / `MotionEvent` / `InputDevice` values an app receives.

Unit tests, golden descriptors, and the fake libusb backend do not replace this.
Record results in [TARGET_MATRIX.md](TARGET_MATRIX.md).
