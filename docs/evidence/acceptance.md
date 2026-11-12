# Recorded agent acceptance and typing sample

This records an earlier real Cursor Agent CLI run on a Samsung Galaxy Note 9
SM-N960F, Android 10, portrait. It is not a new phone test of the public v0.1.0
build, nor a benchmark or compatibility claim for other phones.

Tested source: `2403f08953e2b8efdfade4796fe85ad488874dae`.
Tested executable SHA-256:
`7c15aa4d596874409c6ead4ca939cc42a5fe317fa4274c4e248bbf641678ae64`.

Task: “Open Settings, search for 'Bluetooth', and tell me whether it's on.”
The agent used the official rmcp stdio server and a real native daemon/phone.
It made **11 MCP tool calls, with 0 tool errors**. Tool discovery was separate.
Host-monotonic process-spawn through exit took **87.014398 seconds** (87.014 s
rounded to three decimals); this includes the agent process, not just device I/O.
Bluetooth was **off before and after**; it was not toggled. The final answer
reported Bluetooth off. `session_finish` returned **Phone released**.

## Sanitized tool-call record

| Call | Tool | Seconds | Result |
|---:|---|---:|---|
| 1 | `mobile_launch_app` | 1.310249 | sent via adb |
| 2 | `mobile_list_elements_on_screen` | 2.746130 | 66 elements |
| 3 | `mobile_click_on_screen_at_coordinates` | 0.369494 | sent via aoa-hid/aoa-tracked-session |
| 4 | `mobile_list_elements_on_screen` | 2.482414 | 37 elements |
| 5 | `mobile_type_keys` | 0.219171 | sent via aoa-hid/aoa-tracked-session |
| 6 | `mobile_list_elements_on_screen` | 3.186837 | 19 elements |
| 7 | `mobile_take_screenshot` | 1.830258 | explicit screenshot |
| 8 | `mobile_list_elements_on_screen` | 2.741739 | 44 elements |
| 9 | `mobile_click_on_screen_at_coordinates` | 0.297814 | sent via aoa-hid/aoa-tracked-session |
| 10 | `mobile_list_elements_on_screen` | 2.508740 | 22 elements |
| 11 | `session_finish` | 0.005889 | Phone released |

All four mutation receipts were `sent` with observation `not-requested`.
Sent establishes transport delivery, not application success; the agent separately
observed the Settings result. The `mobile_type_keys` call took **0.219171 s**
(0.219 s rounded). This is one short ASCII action measured by the stdio observer,
not a general typing speed, visual-completion measurement or latency guarantee.
Typed payloads, screenshots, identifiers and local runtime locations are omitted.
