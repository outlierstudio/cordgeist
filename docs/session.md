# Session ownership software unit

`session::Session` is a lifecycle-only facade. Its dedicated owner constructs the
driver and all non-Send handles. `native::session::start` supplies the private
pinned-C adapter. Neither API submits input or owns leases; CLI/MCP do not call
these APIs. The future daemon must own each Session and supply its authoritative
execution guard. Existing immutable grants and kernel action/receipt semantics
are unchanged.

The guard resolves the immutable RouteRef before every initialization phase and
returns owned current route/mapping evidence. Unknown/unverified mapping, wrong
reference, changed exact USB/ADB evidence, changed mapping, AOA <2, cancellation or
an expired initialization deadline prevents further initialization. The adapter
rechecks the remaining budget after validation. Guard implementation must perform
current boot/attachment/fence/health/mapping checks; returning a cached route is
not authorization. There is no default guard or inferred calibration profile.

Initialization opens Context, Device, touch/keyboard Specs, then touch/keyboard
Nodes. Header initializers supply native options. Internal event mode is selected;
logging is disabled and externally forced LIBUSB_DEBUG is refused before Context
creation. Device selection uses the exact bus/address branch explained in
the Session implementation; no interface claim, accessory
switch, USB configuration change or first-device fallback is requested.

Lifecycle phases are Initializing, Ready, Closing, Quarantined and Closed. Ready
means resources acquired, not Android readiness, calibrated touch delivery or
verified keyboard layout. P3 registration settling and P4 measured profiles remain
future work. There is no hidden readiness input or initialization retry.

`ready` and `shutdown` validate caller budgets (nonzero, at most ten seconds).
Cancellation, dropped readiness waiters and last-client drop signal shutdown.
Initialization admission is atomic against shutdown; an already-admitted foreign
call keeps its owner. Timeout/cancellation never detaches or frees its handles.
Cleanup has separate finite native per-call budgets and continues after a caller
times out. Context/Device opens and some upstream multi-request calls have no
whole-call cancellation guarantee; the caller remains bounded while this one
owner can outlive its wait. No automatic replacement owner is launched.

Teardown closes keyboard Node, touch Node, keyboard Spec, touch Spec, Device, then
Context. Pending child cleanup retains its slot, owner and parents and serially
blocks the remaining teardown. Native Node close performs neutralization/drain/
unregister. Every consumed result, even an error, clears its slot exactly once.
Closed readiness is rejected; repeat shutdown reads cached state. Native parameter
or unset-field close errors retain ownership too: these quarantine and park the
owner without automatic retry/free. Process exit is not proof of neutralization.

Status preserves separate initialization and first cleanup failures. Every native
scalar diagnostic is copied before another call can replace TLS; raw strings,
private device identity and input payloads are absent. A Closed phase with a
cleanup error does not assert successful neutralization or permission to resume.
No Session status replaces an action receipt or justifies repeating uncertain
input. Future integration must propagate failures into controller quarantine.

Public software-fake tests exercise ownership/thread affinity, pending retention,
consumed rejection, teardown, cancellation, partial initialization, cleanup errors
and last-client drop. Permanent retained-error quarantine uses an isolated child
process. Real pinned-C ABI tests run only after resolving the marked fake library,
never under real USB. A test-only decorator creates a genuine upstream in-flight
keyboard transfer against fake libusb to exercise pending callback drain, neutral
report bytes and reverse unregister. It does not inject fabricated handle state
or expose production input. The fixture checks finite timeouts, no claims/switch,
no open fake USB handles and no pending transfers at normal completion.

`./scripts/check-native.sh` runs the isolated fixtures with LIBUSB_DEBUG=0 and 4.
`CORDGEIST_DISCOVERY_SANITIZE=ON ./scripts/check-native.sh` additionally checks the
C/C++ fake boundary with ASan/UBSan/LSan, leak detection enabled. Rust sanitizer,
TSan, exhaustive scheduling, crash/unplug and physical-device validation are not
established.
P3 hardware acceptance and G1–G4 remain pending.

## Tracked report extension (software-only)

The next bounded P3 unit adds `Session::submit` on that same owner, with key and
single-contact transitions or a non-nested batch of at most 64 transitions.
Earlier descriptions of the ownership checkpoint as lifecycle-only are historical:
the library now has a guarded report path, still unused by CLI/MCP/daemon.

A single outstanding command reserves admission; overlapping commands fail
QueueFull. Shutdown atomically fences queued work; in-flight native calls retain
handles and cleanup ownership. Caller cancellation and owner cancellation use
separate signals, checked after execution guard validation. Budget checks occur
before admission, after state validation, and after native guard validation;
blocking submit receives a finite nonzero remaining budget capped at 500ms.

Key transitions track held usages; duplicate down/unheld up is refused. A contact
uses ID zero, down while held updates its position, and unheld up is refused.
Batch transition order and native coordinates are validated before the first
report. No ASCII conversion, timing/gesture interpolation or registration settling
is implemented. Modifiers are raw validated usages, not layout/character proof.

Results distinguish NotSent, Sent, Partial (earlier reports completed before a
later failure) and Unknown. Sent proves native completion only. STALL is refused
but leaves upstream pending state: all refused/uncertain report outcomes stop the
Session and close it, never retry or flush it into a later action. Cancellation
or timeout while waiting returns Unknown and signals cleanup; it cannot establish
that the foreign call sent nothing. Cleanup errors remain separately observable
in shutdown status and never rewrite a previously returned delivery outcome.

This library path does not implement action sequence/idempotency receipts or lease
authority; those remain the unchanged kernel/future daemon's responsibility. Its
guard must validate current immutable grant, route, health and mapping. Tests use
explicit SOFTWARE FAKE drivers and isolated marked fake libusb, including native
STALL/short/timeout and cancelled validation. No phone validation is implied.

## Terminal authority failures

`InvalidRequest` from complete payload preflight is harmless: no driver/tracked
mutation or report occurs, and the Session remains usable. Drivers must reserve
that code for payload errors; authority/identity/health failures require their
specific codes. Observed non-payload guard failures permanently fence submission
and automatically start owner-thread cleanup, even when the rejected command
itself sent nothing. Restoring the guard does not revive the Session.

`terminal_error` retains the first terminal report/authority failure while
`cleanup_error` separately retains cleanup failure. Previously returned delivery
results remain unchanged. Pending cleanup retains nodes/parents/owner exactly as
before. This reacts when validation observes invalidation; no independent health
poller/daemon is introduced. Raw held payloads are never exposed by status.

## Bounded gesture extension

`Transition::Gesture` supports tap, hold and straight swipe on the same tracked
Session owner. Coordinates are integer HID-space coordinates; the authoritative
native mapping guard validates the complete generated plan before its first
report. This is no substitute for image-to-HID calibration or frame authority.

Durations are explicit whole milliseconds, 20ms through 10s. Tap and hold send
contact down, dwell and lift. Swipe sends down, sixteen bounded interpolation
segments (including the exact endpoint), and lift. Its integer interpolation
stays within the endpoint rectangle. These are software defaults, not calibrated
phone timings. Tap and hold currently share dwell semantics; no tap classifier or
Android long-press guarantee is implied.

Gesture duration begins after successful contact-down completion. Targets use a
monotonic clock with cancellation/deadline checks during waits. Scheduling and
native latency may make actual duration longer; timing is not hard real time.
The overall operation deadline remains authoritative. If its remaining budget
cannot cover the planned duration, reject before down. A valid 10s payload cannot
fit a 10s caller budget after overhead; it is honestly rejected, never shortened.
No deadline is silently extended to make it fit.

A gesture cannot be nested in a report batch or take over an already-held contact.
Invalid duration, unsupported structure or out-of-range native endpoint is
harmless preflight rejection. Unknown mapping/authority remains terminal as before.
Cancellation/expiry after down initiates the same owner cleanup; uncertain input
or release failure is never retried as a new gesture. Partial preserves earlier
completed reports. A caller timeout may return Unknown while its owner finishes
or cleans up. Cleanup outcome remains separately available from Session shutdown.

No ASCII/layout handling, registration settling, daemon/CLI integration, physical
input or support claim is included. The fixed interpolation/dwell defaults need
real calibration before any device capability is advertised.

## Private ASCII/layout extension

`Transition::Text(ascii::Text::new(layout, payload))` now supplies text to the same
tracked owner, with only the explicit `us-ansi` physical-key policy. Printable
ASCII (space through tilde), Tab and LF are supported. Tab emits usage 0x2B and LF
emits Enter usage 0x28; CR, DEL, NUL, other controls and non-ASCII are rejected.
There is no CRLF normalization, Unicode/clipboard/ADB fallback or layout guessing.

Complete nonempty payloads are validated before their first state mutation/report.
The existing plan limit is 4096 ASCII characters; this byte limit is equivalent
because accepted characters are ASCII. Only Text commands may use budgets up to
120 seconds. Other Session submit/wait/cleanup limits are unchanged. Every native
submit remains bounded by the smaller remaining budget and 500ms, with no retries.

Entry requires no held keys (including any modifier) or contact. Rejected text
never changes inherited held state. Each shifted character presses left Shift,
presses/releases its ordinary key, then releases Shift; plain characters use
press/release. Chords run serialized through tracked state, native execution guard
and descriptor-exact validation. Whole text is preflighted first; per-character
chords remain below the existing 64-transition batch bound. At most 16384 native
report transitions are planned for a maximum shifted payload.

InvalidRequest/UnsupportedCharacter are harmless complete-payload preflight codes;
authority/identity/health failures remain terminal. Cancellation or uncertainty
after a prefix preserves Partial/Unknown and triggers existing close/neutralization,
never retransmits that prefix. Cleanup error remains separate from delivery.

Text and derived transitions have no Debug/Serialize, status content or logging.
Payload/chords are bounded process-memory data, not persisted. No digest is added;
existing kernel payload digests remain keyed and memory-only. This is not secure
zeroization or protection from privileged process-memory inspection.

`us-ansi` is a caller-selected software policy, not detected/verified Android layout.
Caps Lock must be off; IME, dead-key/remapping, focus and physical key interpretation
remain unverified prerequisites. Software fake report bytes do not prove text on a
phone. Registration readiness and daemon/CLI integration remain separately scoped.

## Provisional registration settling

`Session::start` and native `start` default to a provisional 100ms settling
interval after all resources are acquired. `start_with_registration` accepts an
explicit `Registration { settling }`, bounded to 0..5 seconds. Zero explicitly
disables the wait, not guard validation or hardware verification. Values above
the bound reject before driver construction. Initialization budget still includes
resource acquisition and settling; settling never extends the caller's deadline.

Status distinguishes `resources_acquired` (all initialization resources acquired),
`settling_elapsed` (configured interval completed under a valid software guard),
and `hardware_verified` (always false in this unit). These are retained historical
milestones, not promises that handles remain open after Closed. Settling is a
separate phase. Ready now means software settling completed, not proven Android
input readiness. Existing earlier lifecycle-only Ready wording is superseded.

The existing owner checks cancellation/shutdown/deadline and the native authoritative
guard during settling, off the executor, with no synchronization guard held across
foreign calls. Reports reject while Initializing/Settling. Guard failure fences
and triggers the existing ordered cleanup; cleanup bypasses expired authority.
Late validation cannot promote after expiry or restore a closing/closed Session.
Status masks Ready once the closing fence is set. No hidden probe input,
registration retry, input replay, new owner or cleanup workaround is introduced.

Guard checks are bounded at the caller; a blocked foreign call/guard may retain
its owner beyond the caller's wait until it returns. Elapsed delay is no proof of
Android device registration, event delivery, geometry, focus, keyboard layout or
physical effect. Production capability promotion still requires deliberate
hardware evidence; no API here sets hardware_verified true.
