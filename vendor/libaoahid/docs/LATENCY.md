# Latency

The send path is built for bounded, allocation-free work per report. This file
makes no timing claims that were not measured.

## What the implementation minimizes

- One transfer pool is allocated per device before reports are sent.
- Each slot owns its setup packet and maximum-sized payload buffer.
- The free list is LIFO and acquisition is O(1).
- Constant request-57 setup bytes are written once; HID ID and length are
  written per acquisition because the pool is shared by nodes.
- Generated-profile state serializers write directly into the acquired payload
  buffer. Raw reports are copied once because the asynchronous transfer cannot
  depend on the caller retaining its input buffer.
- The optional `validate_reports` pass is a second linear scan of the generated
  wire image; disable it only after the same immutable specs have been tested
  against the target.
- One Input report maps to one control transfer; reports are never fragmented.
- Caller-poll mode creates no **libaoahid** event thread and selects null
  libaoahid Context/Node mutex pointers. It does not remove libusb's own locks
  or any thread created by an operating-system backend. Internal-thread mode
  enables the Context, Node-completion, and transport synchronization needed
  between the caller and libaoahid's event thread.
- The event loop is reaped after async submission so an immediately completed
  request can be observed without a pre-submit delay.
- The transport has no deferred work. The library never retries a report, so `poll` is exactly one libusb event-handling call: no device
  registry, retry scan, or clock read runs on the submit path. This matters on
  the submit path and not only in the poll loop, because caller-poll mode polls
  with a zero timeout immediately after accepting a report.
- The Context graveyard is checked the same way: an atomic size is read without
  the Context mutex, so a poll with nothing awaiting reclamation takes no lock.
- The internal thread uses a bounded 60-second libusb event wait while idle.
  Context teardown explicitly interrupts the active libusb event handler, so it does
  not wait for the idle bound to expire. This is a blocking maximum, not a
  60-second polling interval: an available USB event wakes the handler.
- Relative axes retain 64-bit pending totals and emit bounded field-sized
  fragments; completion consumes exactly the submitted fragment.
- No callback sleeps, and the library never sleeps to retry. A STALLed report
  stays pending for the caller's own resend ([API.md](API.md#submission)).
- An internal event-pump failure is followed by an interruptible 10 ms
  condition-variable wait, preventing an immediately failing backend from
  turning that error path into a busy loop.
- Sticky device removal prevents work from accumulating for a lost link.

## Initial values and their cost model

`aoahid_device_options_init` selects 4096-byte descriptor fragments, 4 pool
slots and a 1024-byte maximum report buffer; `AOAHID_CONTROL_TIMEOUT_MS` and
`AOAHID_SEND_TIMEOUT_MS` are 500 ms and `AOAHID_DRAIN_TIMEOUT_MS` is 1000 ms.
A required value left zero is refused, never replaced. Node reservation
zero/zero means no reservation. These are project choices, not libusb
recommendations or measured optima.

A timeout is a backend failure deadline, not an intentional delay. A successful
control transfer or report completes when the backend reports completion; it
does not wait for the unused portion of the 500 ms budget. Timeout and close
budgets also are not hard end-to-end operating-system latency bounds: backend,
scheduler, driver, controller, and device behavior must be measured on the
deployment host.

The default pool allocates eight payload buffers of `1024 + 8` bytes, where the
eight bytes are the libusb control setup area: 8,256 bytes of slot-buffer
storage. This is a lower-level payload-buffer calculation, not total heap use;
libusb transfer objects, vectors, handles, and allocator metadata are additional.
Using an explicit smaller maximum report or smaller pool reduces this reserved
storage when the application's reports and concurrency permit it. Increasing
the pool does not parallelize EP0.

The 64-byte descriptor fragment setting affects registration request 56 only;
Input reports remain one request 57 each and are never fragmented. A Node with
no reservation consumes no dedicated slot; this saves no Device-pool allocation because the pool is
already allocated, but it leaves all slots shared and changes fairness under
contention.

There is no basis for fixed latency predictions that differ between Linux and
Windows, x86-64 and ARM64, or one libusb backend and another. OS-specific
numbers are only meaningful from a reproducible benchmark on that exact
host/controller/cable/phone/kernel/Android combination.

## Real libusb backend boundary

The deterministic hot-path probe uses the fake backend. It does not instrument
libusb's C allocator, its mutexes, an operating-system API, or a USB host
controller. The following facts come from reading libusb v1.0.30 (commit
`87a55632db62c9bdc58cd31d3ccfa673f1bb017f`); they describe code structure, not
measured latency.

| Boundary | Linux usbfs | Windows WinUSB-like path | What cannot be inferred |
|---|---|---|---|
| Common async core | `libusb_alloc_transfer` uses `calloc` and initializes a transfer mutex. `libusb_submit_transfer` takes `flying_transfers_lock` and the transfer mutex around bookkeeping and backend submission. libaoahid allocates these objects with its pool instead of recreating them per report. | The same common-core allocation and locking apply. Windows-private storage, including `OVERLAPPED`, is part of the allocated transfer object. | Pool reuse does not make real submission lock-free or prove that all allocators are absent. |
| Control submission | `submit_control_transfer` performs a fresh `calloc` of one `usbfs_urb`, then calls `USBDEVFS_SUBMITURB`. | `windows_submit_transfer` updates `active_transfers` under the device-handle lock. `winusbx_submit_control_transfer` passes the embedded `OVERLAPPED` to `ControlTransfer`; an immediate success is posted as a completion and `ERROR_IO_PENDING` completes asynchronously. No explicit `calloc` occurs inside this WinUSB-like submit symbol. | Source structure gives no cost for allocation, lock contention, ioctl/API entry, driver work, or hidden OS allocation. It cannot establish which OS is faster. |
| Completion | `op_handle_events` holds `open_devs_lock`; `reap_for_handle` calls `USBDEVFS_REAPURBNDELAY`; `handle_control_completion` takes the transfer mutex and frees the URB before common completion/callback processing. | Each libusb Context owns an IOCP and `windows_iocp_thread`. The thread waits in `GetQueuedCompletionStatus`, takes the Context and device-handle locks to remove the matching transfer, and signals libusb core. A later libusb event-handler pass runs `windows_handle_transfer_completion` and the callback. | The extra Windows handoff and the Linux reap path identify work to measure, not a numeric scheduler or completion delay. |
| Threads at Context lifetime | On a non-Android Linux host build with discovery enabled, `op_init` starts the build-selected udev or netlink hotplug monitor for the first Context; those monitor implementations create a pthread. The source has a separate Android-application branch that starts neither monitor. This is separate from report-completion polling. | `windows_init` creates the IOCP helper thread for every Context, including when libaoahid uses caller-poll mode. | "No libaoahid event thread" is not a process-wide thread-count claim. A build option, driver selection, or OS revision can change backend behavior. |

Primary symbols and immutable source: [`io.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/io.c),
[`linux_usbfs.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/linux_usbfs.c),
[`linux_usbfs.h`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/linux_usbfs.h),
[`linux_udev.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/linux_udev.c),
[`linux_netlink.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/linux_netlink.c),
[`windows_common.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/windows_common.c),
[`windows_common.h`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/windows_common.h),
and [`windows_winusb.c`](https://github.com/libusb/libusb/blob/87a55632db62c9bdc58cd31d3ccfa673f1bb017f/libusb/os/windows_winusb.c).

## Measurement model

Instrument these boundaries on a release build instead of assigning guessed
constants:

| Timestamp | Boundary |
|---|---|
| `t0` | Entry to the profile mutation/submit call on the host |
| `t1` | Immediately before `libusb_submit_transfer` |
| `t2` | Return from `libusb_submit_transfer` |
| `t3` | Backend completion becomes ready to the libusb event mechanism |
| `t4` | Entry to libaoahid's libusb completion callback |
| `t5` | Android gadget receives request 57, if target tracing exposes it |
| `t6` | The target HID/input layer injects the report, if traceable |
| `t7` | The intended Android consumer observes the input event |

On one host clock, `t1-t0` measures library preparation, `t2-t1` measures
common/backend submission, `t3-t2` measures the backend/device interval, and
`t4-t3` measures host completion dispatch. The end-to-end Android value is
`t7-t0`, but comparing target timestamps `t5` through `t7` with host timestamps
requires a synchronized clock or a separately characterized correlation
method. Do not assume the host callback and Android input processing form a
strictly serial chain merely to make the intervals add up.

Caller-poll mode has one conditional scheduling bound. If a completion is
ready at `t3` and the application contract guarantees entry into the next
successful `aoahid_context_poll` no later than `G` after any such instant, then
the delay from readiness to that poll entry is in `[0, G]`. The opportunistic
zero-timeout event pass after submit can reduce this delay when completion is
already ready, but it does not guarantee zero. Time spent executing libusb's
event path and callback is outside this bound. A missed/failed poll, an
unbounded caller stall, or a definition of `G` based only on average cadence
invalidates the upper bound.

Internal-thread mode removes the caller-controlled `G` term but substitutes
operating-system scheduling of libaoahid's event thread, which has no finite
dispatch-time bound. Likewise, the control/send timeout and close-drain values
are failure budgets rather than success-latency estimates.

## What the tests show

| Check | What a passing test establishes | What it does not establish |
|---|---|---|
| Caller-poll hot-path probe | After prewarming fake event dispatch, one keyboard down/submit/completion and one up/submit/completion make zero calls to the test-instrumented C++ allocation operators. | It does not intercept every possible allocator in a real libusb backend, measure elapsed time, or cover every profile/state transition. |
| Caller-poll synchronization selectors | The tested caller-poll Context and Node have null active mutex selectors, so their update/submit/completion path does not take the library's Context or Node mutex. | It does not make multiple callers safe; the application must still serialize a caller-poll Context. |
| Caller-poll work counters | That same two-report cycle records exactly two successful submissions, two zero-timeout event-handler calls, two callbacks, and no blocking wait in the fake backend. Each completion is observed before the next mutation. | These are operation counts for one deterministic path, not wall-clock latency, CPU load, or a real libusb-backend trace. |
| Internal-thread integration loop | Completion races with Node close for 128 iterations, error publication is transferred to the caller, and failed-thread teardown drains pending work. | A finite schedule cannot prove the absence of every possible race. |
| Linux ThreadSanitizer suite | The full deterministic suite executes with ThreadSanitizer instrumentation and fails if TSan reports an observed data race. | TSan is schedule-dependent and does not prove race freedom; it also does not validate Android hardware or Windows synchronization. |
| Internal-thread idle wait | After one injected immediate backend error, the fake backend observes only nonzero 60-second requested waits, no submissions or callbacks, and no zero-timeout calls while the Context is idle. The error path itself uses an interruptible 10 ms condition-variable backoff. | Counters prove the selected blocking path, not a CPU percentage or scheduler behavior on a physical host. |
| Cancellation and teardown wake | A deliberately delayed transfer blocks a fake five-second event wait; cancellation causes exactly one cancellation wake and terminal callback. Separately, idle Context teardown interrupts a fake 60-second wait exactly once, with no timeout wake and no remaining waiter. | The five-second value is a deadlock escape, not a latency threshold; the test does not benchmark a real libusb backend. |
| Graveyard fast path and no retry | The graveyard counter stays exact across close and teardown, and the STALL test observes exactly one request 57 per submit with the refused state left pending. | Removing retry scheduling is a structural reduction in instructions per submit and poll. It is not a measured latency improvement, and no figure is claimed for it. |
| Bulk Channel hot-path probe | In caller-poll mode, one Channel read of delivered data, one write, one poll, and one empty nonwaiting read make zero calls to the test-instrumented C++ allocation operators. | It does not measure elapsed time or a real libusb backend's allocations. |

Run the race-detection build on Linux with GNU or Clang:

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

The preset enables `AOAHID_TSAN`, the deterministic fake libusb backend, tests,
and examples in a `RelWithDebInfo` build. `AOAHID_TSAN` is mutually exclusive
with `AOAHID_SANITIZE` and `AOAHID_BUILD_FUZZ`. ThreadSanitizer adds substantial
instrumentation and scheduling overhead, so its wall time, CPU use, and
latency must never be reported as release-build performance.

## Choosing the lower-overhead path

Use caller-poll mode when the application already has an event loop and can
serialize every call for the Context. It creates no library event thread and
selects the lock-free Context/Node synchronization path described above. The
application must call `aoahid_context_poll` often enough for completions and
teardown to progress; a long polling gap directly adds completion latency.

Use internal-thread mode when independent event progress is more important
than avoiding one thread and its synchronization. Blocking waits use condition
variables or libusb's blocking event handler rather than repeatedly checking
completion state. The idle event wait is bounded at 60 seconds and explicitly
interrupted during teardown. This mode is tested under ThreadSanitizer, but it necessarily has more scheduling and lock work
than caller-poll mode.

Keep specs and Nodes alive instead of rebuilding them per report. Choose the
smallest transfer-pool and maximum-report policies that cover the application,
because larger limits reserve more memory but do not make EP0 parallel. Leave
logging disabled on latency-sensitive deployments. `validate_reports=1` adds a
second linear validation scan; disable it only after the same immutable specs
and reports have been tested against the target.

## Bulk Channels next to HID

Bulk Channel transfers complete on the same libusb event path as HID
completions. Each Bulk completion stores a slot index in a
single-producer/single-consumer ring, decrements an atomic in-flight count, and
wakes a waiting thread; it takes that waiter's mutex only while a reader or
writer is actually blocked, the pattern Node completion already uses. Bulk
transfers use their own pool, so a full Bulk pool never consumes a HID slot, and
Channel read/write allocate nothing. Whether heavy Bulk traffic, such as a large
`adb push`, raises HID latency on real hardware has not been measured yet
([TARGET_MATRIX.md](TARGET_MATRIX.md)).

## Measuring on the target

No nanosecond cost, jitter bound, or throughput number is stated without a
reproducible benchmark, compiler, CPU, libusb backend, and percentile report.
The library makes no promise that increasing pool size creates parallel EP0 wire
transfers. Pool size controls ready-work capacity, fairness, and callback
lifetime.

For caller-controlled scheduling, select caller-poll mode, keep all calls for
that context on one scheduling domain, prebuild/retain specs, and submit only
state already validated by its profile. Measure latency and jitter on the actual
host, USB controller, cable, phone, kernel, and Android build.
