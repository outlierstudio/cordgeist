# Runnable CLI

Cordgeist’s command line talks to one local daemon over a private Unix socket.
It does not open a network API, and it does not repeat a sent action under a new
sequence. Recorded hardware evidence is phone/source-specific; availability and receipts do not establish application success.

## Build

From a checkout with the pinned toolchain:

```sh
source scripts/env.sh
CARGO_NET_OFFLINE=true cargo build --locked --offline
```

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | The command completed and the daemon returned success. |
| 2 | The command was rejected or a required prerequisite is unavailable. |
| 3 | The request was written, but the reply was lost or the daemon reported `process-timeout`. Inspect `action status` for that same sequence. |

`status` exits 0 for current native availability, including clean rearmable
hand-back. Read-only `doctor` exits 0 when its nine prerequisites pass. Neither
means calibrated accuracy or verified application success. The labeled software
backend does not establish native readiness.

`--json` prints one JSON value on stdout. Diagnostics stay out of stdout in that
mode. Human mode prints a short result on stdout and the recovery line on stderr.
A lost reply or `process-timeout` is exit 3. Human mode prints the action
sequence on stderr when it has one. A lost acquire does not write a lease file.
JSON redacts `credential`, `text`, `base64`, `serial`, and `product`.

## Runtime

`--runtime DIR` is the private parent directory (mode `0700`) in which the daemon
creates `cordgeist/`. Otherwise the CLI uses an owned `0700` `XDG_RUNTIME_DIR`.
The daemon holds `cordgeist/instance.lock` and publishes `daemon.sock` only after
initialization. A non-socket path at `daemon.sock` is rejected and left in place.

`daemon start` is deliberate. `--autostart` on a later command starts that same
daemon if it is not already running. `daemon stop` asks the locked process to
shut down. No login or systemd unit is installed.

## Single-phone workflow

```sh
cordgeist --runtime "$RUNTIME" daemon start
cordgeist --runtime "$RUNTIME" doctor
cordgeist --runtime "$RUNTIME" acquire --device ID --reason "why" --lease-file "$RUNTIME/lease.json"
cordgeist --runtime "$RUNTIME" screenshot --lease-file "$RUNTIME/lease.json" --output "$RUNTIME/shot.png"
cordgeist --runtime "$RUNTIME" tap --lease-file "$RUNTIME/lease.json" --frame FRAME --x X --y Y
cordgeist --runtime "$RUNTIME" action status --lease-file "$RUNTIME/lease.json" --seq N
cordgeist --runtime "$RUNTIME" release --lease-file "$RUNTIME/lease.json"
cordgeist --runtime "$RUNTIME" daemon stop
```

`list` (alias `inventory`) shows identity labels, not secrets. `swipe`, `hold`,
`key`, and `action cancel` use the same lease file. `type` requires `--stdin`.
The process reads the corpus without echo when stdin is a terminal and never
logs the bytes.

`setup calibrate --device ID` prepares the exact phone's synthetic-page USB
loopback route and reads axis evidence. `--probe` explicitly verifies three
owned screenshot targets, neutralizes and stores the per-phone mapping.
Default `setup` uses generic geometry without calibration input. Optional
`setup calibrate --verify` retains the explicit synthetic verifier; unsupported
optional parser output warns. Failed target verification is never a pass.
Unknown geometry, missing owned markers and uncertain input remain fail-closed.
Recorded measurement scope is summarized in [measurements](measurements.md).

`status` and `doctor` return 0 only for currently valid mapped native readiness.
That does not complete the broader release hardware gates.

## Screenshots

The PNG is written to a temporary file in the destination directory, renamed into
place, and only then followed by `NAME.meta.json`. Those two files are not one
filesystem transaction. A PNG without its sidecar is an interrupted export and is
not a successful screenshot. Existing destinations are left unchanged.

## Lease files

Acquire creates a mode `0600` lease file. The credential stays in that file.
Sequence numbers are allocated under an exclusive lock. A sequence is consumed
only after the request bytes are written. A timeout or lost reply records that
sequence and does not allocate another one for an automatic retry.

Paths must be absolute, must not contain `.` or `..`, and must not be symlinks.
The parent directory must be mode `0700` and owned by the user. Any other mode
is rejected.

## Labeled software backend

A development build selected with `--features fake` and
`CORDGEIST_SOFTWARE_BACKEND=labeled-software-only` installs one explicitly
synthetic phone named `software-phone`. That backend is `fake-software-only`.
A production build refuses the variable and does not install a phone. Neither
build is evidence of touch delivery or calibration.

## Setup

For the beginner setup path, set CORDGEIST_ADB to your installed adb executable
and run `cordgeist --runtime "$RUNTIME" setup`. Device selection and calibration
belong to the daemon. It revalidates saved profiles and reports the next physical
step when needed. Explicit diagnostic calibration accepts an optional --device;
omitting it requires exactly one phone. See [the README](../README.md) for the full flow.

Current release/v1 action observation policy: CLI actions default to receipt-only,
without post-action capture. Pass global --observe-after to opt in. The daemon
Request.observeAfter field defaults false and is part of mutation replay identity;
MCP input tools expose observe_after. Explicit screenshot remains independent.
Live route/frame/geometry validation and cleanup remain mandatory.

Default `cordgeist setup` selects the single connected phone through the daemon
and reports readiness using generic geometry. It does not serve a page or send
calibration taps. Installed `adb` is resolved on PATH; an absolute CORDGEIST_ADB
override remains supported. `cordgeist setup calibrate --verify` opts into the
owned synthetic-target verifier (`--probe` remains an alias). Open the deliberate
synthetic page first. Unsupported optional input-mapper parsing produces a
warning; a failed target verification is not a successful calibration. Saved
verified legacy profiles retain their measured mapping.
