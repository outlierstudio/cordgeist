# Cordgeist

Cordgeist is an MIT-licensed Linux CLI, daemon and local stdio MCP server for controlling one Android phone over USB AOA HID.

## Why HID

One daemon registers USB AOA 2 touchscreen and keyboard devices. Touch, swipe,
hold and ASCII typing use HID. ADB supplies screenshots, UI elements, app launch
and explicitly labeled BACK/HOME/ENTER keyevents. CLI and official rmcp stdio
clients share exclusive ownership, frame validation and truthful action receipts.
App detection behavior and equivalence to a physical finger are unmeasured.

## Install a Linux archive

Requirements: Linux x86_64, glibc 2.31 or newer, installed Google platform-tools
`adb`, and USB permission for one unlocked, debugging-authorized phone. The
portable archive includes replaceable shared libaoahid and libusb 1.0.30 plus
licenses, corresponding source and rebuild instructions. It does not bundle ADB.
Ubuntu 22.04 loader/diagnostic results are recorded separately from phone tests.

Given the archive and its checksum in the same directory:

```sh
sha256sum -c cordgeist-v0.1.0-linux-x86_64.tar.gz.sha256
mkdir -p "$HOME/.local/opt"
tar -xzf cordgeist-v0.1.0-linux-x86_64.tar.gz -C "$HOME/.local/opt"
APP="$HOME/.local/opt/cordgeist-v0.1.0-linux-x86_64"
"$APP/cordgeist" --version
```

For upgrades, close older Cordgeist MCP connections and stop their daemon first.
Do not run multiple native daemons for the same phone.

If doctor reports USB permission errors, review the scoped rule and explicitly
install it yourself:

```sh
sudo install -m 644 "$APP/packaging/51-cordgeist.rules" /etc/udev/rules.d/51-cordgeist.rules
sudo udevadm control --reload-rules
```

Reconnect the phone and unlock it. No installer runs sudo or changes drivers.
The normal-mode rule covers the tested Samsung USB ID; other normal USB IDs need
an explicit local rule. AOA accessory IDs are covered independently of model.
This permission scope is not a setup model allowlist. See packaging/UDEV.md.

## Setup and Cursor

Connect only the owned phone, enable USB debugging and approve the host key.
Leave a deliberate test screen visible for initial checks.

```sh
RUNTIME="$HOME/.local/state/cordgeist"
install -d -m 700 "$RUNTIME"
"$APP/cordgeist" doctor --json
"$APP/cordgeist" --runtime "$RUNTIME" setup
realpath "$APP/cordgeist"
```

Setup starts the daemon and selects the exact single attached phone. Generic
mapping uses current effective wm size, display geometry and the registered HID
descriptor. Calibration is optional. Ordinary doctor is read-only; no tap test.
Readiness means the native backend can be acquired, including clean hand-back
that can be rearmed. It does not imply an owned lease, fresh UI frame, calibrated
accuracy or application success. Capability `detected` is not `verified`.

Use the absolute executable and runtime paths in Cursor's MCP configuration
(JSON does not expand `$HOME`):

```json
{
  "mcpServers": {
    "cordgeist": {
      "command": "/absolute/path/to/cordgeist-v0.1.0-linux-x86_64/cordgeist",
      "args": ["--runtime", "/absolute/path/to/private-runtime", "mcp"]
    }
  }
}
```

Run setup before connecting Cursor. Disable older Cordgeist entries and open a
fresh chat. No lease credential or lease file is exposed to the model.

If Cursor reports `backend-unavailable` while the intended daemon is ready,
inspect the Cordgeist entry in `~/.cursor/mcp.json`. In the measured Cursor CLI
run, a stale global entry selected an older executable and a missing daemon
socket despite the prepared workspace entry. Check both the absolute `command`
and its `--runtime` path, then restart the MCP server. A standalone doctor pass
does not prove that Cursor selected that same executable or daemon.

A typical workflow:

1. `mobile_launch_app({"appName":"Settings"})` opens Settings directly.
2. `mobile_list_elements_on_screen` returns untrusted labels and `item.element`.
3. `mobile_click_on_screen_at_coordinates({"element": item.element})` computes
   the center on the server. List again to observe the result. Coordinates are
   fallback only, in SCREEN pixels; legacy tools retain screenshot-pixel contracts.
4. Use explicit screenshots for empty/visual screens. An unavailable element dump
   returns an explicit error and fresh screenshot fallback, with bounded work.
5. Finish with `session_finish`. Idle/session-end expiry are cleanup backstops.

Names resolve through framework roles (Settings, Camera) or a unique installed
package suffix. Full localized label lookup is not implemented. Unknown or
ambiguous names require an exact `packageName`; only Settings role launch has
phone evidence. No manufacturer-specific package table is used.

Actions return receipts without automatic post-action screenshots. `sent` means
transport delivery; verify application effects with a new observation. After an
uncertain mutation, resolve its original status; never replay with a fresh identity.

## Measured phone and accuracy

| Phone | Android | Mode | Recorded evidence |
|---|---|---|---|
| Samsung Galaxy Note 9 SM-N960F | 10 | USB, portrait, effective 1080×2220 | Nine read-only doctor checks passed; real Cursor owner reports 13 calls, 0 misses and hand-back; recorded nine-point perimeter max Android-mapped error 1.343 px |

The Cursor result above predates the status/name fixes; its wall time was not
provided. The measurements describe earlier development builds, not hardware acceptance
of this public source snapshot. Do not infer a numerical speed improvement. Perimeter
numbers use observed kernel HID events and Android InputReader mapping; the
independent rendered-marker inset-grid maximum was 0.707 px. These do not establish
subpixel physical precision. Other phones and landscape accuracy are untested.
See [measurement scope](docs/measurements.md).

## Limits and privacy

- No Bluetooth HID transport in v1; the input connection is USB.
- One phone and one owning client at a time; other clients receive a busy/leased result.
- Portrait is the measured mode. Other rotations warn that taps may be rotated;
  landscape accuracy is unverified. Unknown geometry and stale authority reject.
- ASCII US-ANSI typing only. No OCR, streaming, dashboards or media MCP tools.
- BACK/HOME/ENTER use authorized ADB keyevents and report `via: adb`.
- UI text is untrusted data. Screenshots and element content requested through MCP
  reach the selected client/model provider. Editable/password values are withheld
  from element results, but screenshots can contain sensitive content.
- Cordgeist does not silently record screenshots. Explicit CLI exports and the
  opted-in hardware measurement helper have their documented local files/cleanup.
- Optional calibration targets must be on owned synthetic content. Parser failure
  warns; failed verification never becomes a pass. Saved profiles stay phone-bound.
- The udev rule is prepared for explicit installation; it was not installed by
  the release executor. Loader checks do not prove USB access on another host.

## Development and credits

Use the pinned Rust toolchain and offline dependencies:

```sh
source scripts/env.sh
export CARGO_NET_OFFLINE=true
./scripts/check.sh
./scripts/build-native.sh
./scripts/check-native.sh
```

Native source builds require CMake ≥3.20, C++20, Ninja, pkg-config and matching
libusb headers/runtime ≥1.0.30. Build scripts do not fetch dependencies. See
[portable rebuild instructions](packaging/PORTABLE-BUILD.md).

Cordgeist is MIT. Credits: pinned libaoahid (MIT), mobile-mcp ideas ported to Rust
(Apache-2.0), official rmcp (Apache-2.0 with supplied notices), quick-xml (MIT),
and bundled libusb (LGPL-2.1-or-later). Full terms and source obligations are in
LICENSE, NOTICE, THIRD_PARTY_NOTICES.md and third-party-notices in the archive.
No claim is made that apps cannot distinguish HID from a finger.

App launch accepts `appName` such as `Chrome`, `Settings` or `Camera`; unknown
or ambiguous names require an exact installed `packageName`. Launch has a
separate 4-second budget, based on five Note9 cold command timings (median
0.568181s, maximum 0.896441s) with margin for validation and resolution.
These are command-exit timings, not visual page-load measurements. See
[measurement scope](docs/measurements.md).
