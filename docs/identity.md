# Discovery and identity (P2.3 software)

These are internal library paths for the future single daemon owner. Bootstrap
CLI/MCP do not create a second controller or expose phone input. No phone/host
combination is validated; discovery/capability detection is not calibration.

## Owner and deadlines

`native::discovery::worker()` (native-aoa feature) creates a separate serialized
Context owner thread. `DiscoveryWorker::snapshot` admits one outstanding request,
returns owned `UsbDevice` copies and labels the discovery source. No raw native
handle/string crosses the worker boundary. Native discovery only probes AOA
request 51 in the current configuration; it never creates a Device/Node, registers
HID, claims an interface or switches USB mode. Normal tests never probe real USB.

Caller budgets are nonzero and at most ten seconds. The pinned C API's probe
budget is per device, not total scan time; standard descriptor reads use libusb's
timeouts. If the caller expires/cancels/drops, late results are discarded and the
worker closes/quarantines. Pending Context close stays on the owner and is retried;
all other close results consume the handle and are never retried. Shutdown has a
bounded wait and reports pending cleanup truthfully. A blocked foreign call or
permanently pending cleanup may retain that worker until process exit; it cannot
safely be killed. No automatic replacement worker/retry is implemented.

Externally forced LIBUSB_DEBUG is refused before Context creation unless set to 0.
Raw discovery/identity structures intentionally lack Debug; do not log their serials,
products, pairing files or copied ADB inventory. Test artifacts are separately marked
software fakes, never installed into the production prefix and never device proof.

## Exact routes and generations

`DiscoveryService::refresh` invalidates existing route references before awaiting
anything. It loads explicit pairings, reads the configured ADB server inventory,
gets the owned native snapshot, reads ADB again and verifies unchanged pairing
data. It checks the global deadline and cancellation again before publishing.
Malformed/missing/ambiguous evidence, timeout, cancellation, configuration changes,
reconnect or failure leave old routes unusable. Filesystem loads have one bounded
outstanding slot even if a cancelled caller leaves a blocking read finishing.

`IdentityRegistry` requires matching USB topology and one exact current ADB numeric
transport ID. Automatic association additionally requires matching nonblank serials
unique in both inventories. Duplicate transport IDs, duplicate topology, nonblank
serial disagreement, unauthorized/offline/unsupported states and unknown USB
routing fail closed. An error currently invalidates the whole refresh; per-device
fault isolation/health propagation is future daemon work, not a claim here.

Blank/duplicate USB serials require an explicit port association with exact port,
VID/PID and observed USB/ADB serial evidence. An explicitly paired port movement or
evidence change requires re-pairing; it does not silently become an automatic ID.
A port association identifies a deliberately assigned port, not a cryptographic
phone identity: indistinguishable devices swapped on the same ports cannot be
proved distinct by these snapshots. Reconfirm physical setup before control.

Every successful refresh issues fresh attachment generations, even with identical
evidence. Polling cannot prove continuous attachment between scans, so it cannot
safely preserve an existing lease. Old boot/device/generation references fail
`resolve`; every service restart chooses a fresh boot ID. Route evidence expires
after five seconds. A resolved route is a copied target, not authorization. The
future daemon must resolve immediately before work and propagate invalidation to
leases, frames, native ownership and health. P1 immutable grants are never amended
or renewed by this module. Unknown geometry/mapping remains unsupported for input.

## Explicit persistence

On Linux, `PairingStore::open` requires an already-created absolute, owned 0700
directory with no symlink components. Files are regular, owned 0600 single-link
files. Directory-FD-relative I/O, a nonblocking lock, 64 KiB limits, strict version
and field validation, operation-owned temporary files, fsync and atomic rename
protect updates. Corrupt/unsupported data is an error, never empty fallback or an
implicit overwrite. Post-rename directory-sync failure reports `CommitUncertain`;
reload before deciding whether to retry. Process interruption can leave an owned
temporary file; loading ignores it and never deletes unrelated files.

Only explicit `Pairing::confirm` -> `PairingStore::set_pairing`, `set_alias` and
`remove_pairing` change stored pairing data. Setup must obtain deliberate physical
confirmation before asserting port association. Automatic discovery never persists
inventory. A read may create the private lock file. Aliases are human labels only;
duplicate alias lookup fails rather than choosing the first device. No lease,
credential, action, frame or image is stored. Keep pairings.json private and out of
Git; .gitignore covers its fixed filename/lock/temp files. General configuration
remains separate; decision 0003 explains the scoped JSON/TOML deviation.

## Verification boundary and next work

Public tests exercise the real worker/service/registry/store paths with software
fake USB drivers and executable ADB fixtures. An isolated marked fake-libusb
process also exercises actual C ABI Context/discovery/snapshot/destruction and
checks the transfer ledger for discovery-only behavior. ASan/UBSan/LSan cover that
native path; LeakSanitizer requires an untraced environment. These are software
checks only. They do not prove live USB permissions, Linux ADB/AOA concurrency,
phone identity under physical swaps, HID delivery, calibration or crash cleanup.

The P3 [lifecycle-only Session](session.md) now owns private Device/Node handles
and models consuming close with software-fake evidence. Input/report state,
registration readiness and the remaining native/hardware fault campaign are still
pending. P5 must integrate these primitives into a single owner daemon, propagate
health/generation changes, serialize leases and enforce execution-time authorization.
No new CLI discovery/control command, daemon, IPC service or phone action is enabled.
