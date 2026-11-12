# Linux setup and diagnostics

The v1 release candidate targets Samsung Galaxy Note 9 SM-N960F, Android 10,
build QP1A.190711.020.N960FXXU4DSLB, portrait primary display only.
S24 Ultra validation is deferred to v2. Start with [installation instructions](../README.md).
For source builds, install pinned Rust and build/check as in README. Native prerequisites and local prefix settings are in
[dependencies.md](dependencies.md). Install Google platform-tools separately;
Cordgeist never silently downloads/selects an ADB binary.

Set CORDGEIST_ADB to its installed absolute path, then run `cordgeist setup`.
The daemon selects the one directly connected phone, validates a saved profile
or guides you to the owned calibration page, and completes setup without manual
daemon restarts. No phone-model allowlist is used in normal setup; the release
compatibility statement describes measured evidence only.

`cargo run --locked --bin cordgeist -- doctor --json --adb /absolute/path/to/adb`
checks that configured executable's version and bounded inventory against local
127.0.0.1:5037 (override --adb-port). Discovery sends no phone input. Credentials,
phone serials and raw subprocess stderr are omitted from doctor output. Status and
doctor return 2 until a currently validated native phone is ready.

Unauthorized: unlock the phone and deliberately approve the host key. Offline:
inspect cable, authorization and phone state. No permissions: inspect the exact
USB device's ownership and review targeted udev rules; do not run as root to hide
errors. Missing usbfs in a container requires deliberate passthrough or a physical
Linux environment. An absent /dev/bus/usb here does not prove no phone exists on
the host. An ADB server/interface conflict needs diagnosis and a deliberate
handoff; no command kills the global server or installs drivers/rules.

Before live input, choose an owned unlocked phone and a harmless visible
test area. Establish exact USB/ADB identity, layout and calibration first. Keep
phone data/screenshots/serials out of Git. The broader G1–G4 acceptance remains separate from this narrow Note 9 beta.

Native discovery and handles belong to the daemon owner-worker path.
The native factory refuses externally forced `LIBUSB_DEBUG` values other than 0
before creating a Context: upstream warns that this variable can override its
context-local logging suppression. Clear it or deliberately set 0 for future
native setup. This precondition protects serial/content redaction; it does not
establish USB permissions, AOA input readiness, calibration or hardware support.
