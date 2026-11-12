# Local API contracts and software service

[contracts.json](contracts.json) is generated from the Rust DTOs and catalogue.
It contains versioned request/response envelopes and typed results for all planned methods, action admission,
lease grants/authentication, frame provenance, generic/optional calibrated transforms, receipts,
capabilities, partial media states, and structured errors. Run
`./scripts/generate-docs.sh --check` to detect drift. The catalogue coverage test
compares every method against both request and result schemas, roundtrip fixtures,
the public documentation table.

The production Service dispatcher joins discovery, canonical leases, scheduling,
the native owner-thread Session, observations and truthful receipts over the
private socket. CLI and official rmcp stdio share this daemon. Software-injected
backends remain explicitly labeled tests; they do not prove physical delivery.
Availability is derived from the exact attached native phone and usable mapping,
including clean rearmable hand-back. Capability detection is not calibration or
application verification. A catalogue entry alone is not a hardware capability.

Implemented framing: four-byte big-endian JSON length, ceiling 8 MiB before allocation,
request version/ID/deadline, matching response ID and boot. The Service embedding
owns authority and the existing Session owner threads; clients never instantiate
a second controller.

All mutations require lease-scoped actionSeq. Matching authenticated retries
return original receipts before validating stale frames; mismatches fail. Evicted
sequences fail receipt-expired. Rejected validation never reserves the sequence.
A successful backend submission means sent, not an observed application effect.
Partial or unknown submission and pending cleanup quarantine new actions. An
observation failure must preserve input status and must not repeat input.

Input coordinates are finite fractional zero-based returned-image pixels; endpoints
map to the calibrated HID endpoints with nearest-integer rounding. Every visual
input, including key/type, needs frame provenance. Profiles and geometry are
explicit; no rotation is inferred from aspect ratio. A fresh frame does not prove
unchanged UI or continuing keyboard focus.

Credentials and typed payloads intentionally do not implement Debug. Receipts carry
no payload/credential; keyed digests stay in memory. The current synchronous core
is a P1 admission kernel, independent of the integrated Service. M1 Service tests
now establish active duplicate joining, canonical ownership, frame/receipt budgets,
expiry cleanup, observation completion and priority cancellation through the
existing actor/Session paths. Neither kernel nor service fake tests prove a phone.

P1 response/result contracts are now defined. Wire `Response` contains numeric
`version: 1`, the exact `requestId`, daemon `bootId`, and one `outcome`:

```json
{"version":1,"requestId":"synthetic-request","bootId":"synthetic-boot","outcome":{"status":"success","result":{"method":"devices.list","value":{"devices":[]}}}}
```

An error instead carries `{"status":"error","error":{"code":"backend-unavailable",
"phase":"preflight","recovery":"Configure deliberately"}}` in `outcome`, with no
result. Unknown response versions, conflicting outcomes and unknown envelope fields
are rejected by deserialization. `Response::for_request` constructs correlated replies;
clients must call `validate_for(request, expected_boot)` on decoded responses before
accepting them. It rejects wrong request IDs, boot IDs and success-result methods.
Unsupported request versions can receive a version-1, correlated
`protocol-version-unsupported` error; they cannot receive a success through this API.
This helper is also used by the implemented private IPC handshake/dispatcher tests.

Envelope success means a typed operation result was delivered, not that application
input succeeded. Receipts retain sent/partial/unknown input, observation outcome and
cleanup independently. A cancellation request can remain requested with cleanup
pending; it does not imply neutral state or a new lease. Copied media does not imply
indexing or picker visibility. Active action status has no final receipt.

Only `leases.acquire` returns a credential. The envelope/result/lease grant types
intentionally lack Debug, including nested screenshot/OCR data. Normal result shapes
have no credential or original typed-text fields. The private acquire response still
serializes its credential for the intended owner; never log or export this response.
The schema is a wire contract, not a general secret scrubber: producers must redact
free-text errors/recovery/reasons and never echo untrusted payloads into them.

Screenshot results carry image/png base64 content plus frame metadata, never only a
local path. OCR words carry returned-frame bounds and provenance. Evidence remains
unavailable/detected/verified per capability; a detected transport is not verified
input. Synthetic test evidence is explicitly software-only.

M1 implements framing/deadlines, canonical lease-authorized dispatch, bounded PNG
content, receipt/observation separation and trusted profile/publication provenance.
OCR confidence/bounds, media import and real device/profile acceptance remain
pending. Software injection never sets physical capability evidence to verified.

A request deadline is a relative service execution budget, at most 1500ms in this
unit. Server passes its original absolute 2s exchange deadline (started before
handshake) through Dispatcher into Service. The effective operation deadline is
the earlier of that Instant and the validated execution budget; request framing,
admission, action/capture execution and receipt waits cannot renew it. A deadline
can end the caller's wait while owned native cleanup/completion continues. On
client timeout/disconnect, retain the
original lease/sequence and query actions.status or repeat the identical request;
never allocate a fresh sequence to guess whether input occurred. Matching
previously admitted calls look up their receipt before validating frame freshness.
Eviction returns receipt-expired; it does not authorize replay. Release/revoke
report the retired authority fence and current cleanup outcome; a final action's
cleanup field is its completion-time snapshot. Explicit fresh software reconnect
requires confirmed old cleanup, new attachment/grant and observation. No automatic
retry, profile verification, live native installation or arbitrary path export is
provided here. Unknown mapping and changed geometry fail closed.

## Request/result mapping

Each result is an `OperationResult` variant tagged with the originating method.
The payload is in `value`; requests retain their existing `call`/`params` shapes.

| Method | Request parameters | Typed result payload |
| --- | --- | --- |
| `system.status` | none | StatusResult: software version, backend, readiness, evidence |
| `system.doctor` | optional device | DoctorResult: checks/recovery and evidence |
| `devices.list` | none | DevicesResult: identity generations, health, evidence, owner label |
| `leases.acquire` | device, reason, label, TTL | LeaseGrant: private auth, idle deadline, next sequence |
| `leases.release` | auth | LeaseEndResult: resulting fence and cleanup |
| `leases.revoke` | exact device | LeaseEndResult: optional ended lease, fence and cleanup |
| `observations.screenshot` | auth, longest edge, settle budget | ScreenshotResult: PNG content, frame, optional settled flag |
| `input.tap` | auth, sequence, frame, point | ActionResult: receipt and optional observation |
| `input.swipe` | auth, sequence, frame, endpoints, duration | ActionResult |
| `input.hold` | auth, sequence, frame, point, duration | ActionResult |
| `input.type` | auth, sequence, frame, text | ActionResult; never echoes typed text |
| `input.key` | auth, sequence, frame, key | ActionResult |
| `actions.status` | auth, sequence | ActionStatusResult: queued, executing, or final action |
| `actions.cancel` | auth, sequence | CancelResult: requested/cancelled/already-final plus cleanup |
| `observations.text` | auth, optional frame, language | TextResult: frame, language, words/confidence/bounds |
| `media.push` | auth, sequence, source, kind | MediaPushResult: receipt and independent copy/index/picker states |

`setup.calibrate` is local-only daemon setup. It reserves exclusive ownership
while reading bounded Android axis evidence, then releases ownership. A result
with verified=false is pending (CLI exit 2), not calibration acceptance.
Independent target checking is required before private profile storage.

Calibration's optional `probe` flag defaults to false. `--probe` requires the
owned synthetic page and checks screenshot marker positions before frame-bound
input. Its setup exchange has a separate maximum 60-second budget (four bounded
10-second observations plus axis discovery and cleanup); each touch retains the
ordinary finite input cap. `--budget-ms` can shorten setup. Successful target observations and neutral cleanup permit private profile storage.
Restart revalidates the same phone/build, descriptor settings, display and live
axes before ordinary control becomes ready. Note 9 development target evidence
is recorded in progress; S24 Ultra release gates remain pending.

## Selected mobile operations

`observations.elements` is a leased, bounded UI-XML observation. It returns
untrusted labels/rectangles in screen pixels and a geometry-only frame token;
that token is not a screenshot or PNG. Legacy screenshot-pixel input cannot
consume a geometry-only token. `apps.list` returns validated package IDs, with
appName equal to packageName. Neither returns owner credentials through MCP.

`input.android` is a typed launch-app or BACK/HOME/ENTER command. It uses the
same authority, original deadline, sequence/replay records and owner-thread
queue as HID. Non-coordinate controls require exact route/authority but no
image; they retire prior UI provenance. Native transport is explicitly adb.
No arbitrary shell or ADB touch/swipe/typing request exists. ASCII submit=true
keeps typing plus ADB ENTER under one mutation identity; partial/unknown stays
partial/unknown. Ordinary typing remains HID.

Request.coordinateSpace defaults returned-image-pixels for existing clients.
Mobile inputs explicitly select screen-pixels. The daemon translates against
its current validated frame/display; invalid/missing/stale geometry rejects.
Mobile screenshot requests native-size output; mismatched/oversized grids fail
rather than telling agents to guess scaling. Legacy longest-edge1280 stays
downscaled. Raw processing is separately bounded; returned PNG retention limits
remain enforced. Post-action screenshots stay default-off.

Generic mapping uses the daemon's effective `wm size` (override preferred),
current display rotation and the registered descriptor's independent X/Y maxima.
Returned-image coordinates scale by screen/image dimensions before HID conversion.
Mobile screen coordinates translate explicitly through that same geometry. Old
verified profiles retain their original endpoint mapping; their calculations are
not rewritten. Generic mapping is detected capability, not calibrated evidence.
Explicit fresh observations can refresh generic geometry and retire old frames;
receipts and sequence high-water marks survive. Rotation outside zero warns that
landscape is experimental and taps may be rotated. Unknown or ambiguous geometry
still fails closed. Optional input-mapper parsing is not a normal setup gate.
