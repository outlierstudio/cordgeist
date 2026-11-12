# Recorded measurement scope

These results describe earlier development builds on one Samsung Galaxy Note 9
SM-N960F, Android 10, portrait, effective display 1080×2220. They are not new
hardware acceptance of this public source snapshot. Other phones and landscape
accuracy remain untested. S24 Ultra testing is deferred.

The earlier real Cursor run reported 13 calls, zero misses and successful
hand-back. Its wall time was not recorded; no numerical speed improvement is
claimed. The nine-point perimeter measurement used observed kernel HID events
and Android InputReader mapping, with a maximum Android-mapped error of 1.343 px.
The independent rendered-marker inset-grid maximum was 0.707 px. These results
do not establish subpixel physical precision or equivalence to a finger.

Five Chrome cold command samples, in order: 0.568181, 0.674605, 0.896441,
0.479538 and 0.567618 seconds. Each force-stopped Chrome before the timed
launcher command; timing was host monotonic time from process spawn to command
exit. Force-stop was outside the interval, no warmup was discarded, and all
five samples succeeded. Median/p50 is the middle sorted value: 0.568181 s;
maximum 0.896441 s. The 4-second launch budget is four times that maximum rounded
up to whole seconds, with room for validation and resolution. These are command
exit measurements, not visual rendering or page-load measurements.

Earlier portable builds were inspected with objdump and exercised in Ubuntu
22.04 without a phone. Those loader checks do not prove phone compatibility,
and do not certify a newly built public artifact. Inspect every new executable
and bundled native library and repeat loader checks for any distributed archive.

## Later recorded acceptance and typing

A separate later Cursor run recorded 11 MCP tool calls, zero tool errors,
87.014398 seconds process-spawn through exit, Bluetooth off before/after and
successful session_finish. Its short ASCII typing call took 0.219171 seconds.
See the [sanitized acceptance record](evidence/acceptance.md) for source/binary
identity, per-call durations and measurement boundaries. The earlier 13-call run
above is distinct. Neither result was rerun for this public source snapshot.
