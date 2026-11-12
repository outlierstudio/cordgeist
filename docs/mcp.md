# MCP adapter

`cordgeist mcp` is the official rmcp stdio server. It uses the same local client
and daemon as the CLI. It does not open a second USB owner. Tool names and
effects come from the shared catalogue. `leases.revoke`, `daemon`, and `setup`
stay local.

```sh
cordgeist --runtime "$RUNTIME" daemon start
cordgeist --runtime "$RUNTIME" mcp
```

The server speaks newline-delimited JSON-RPC on stdin/stdout. Each connection
privately owns its daemon lease and action sequences. Models never receive or
supply credentials. First ownership-requiring operation acquires atomically;
other sessions/CLI clients receive leased. Use session_finish at task end;
orderly connection shutdown and bounded idle/daemon expiry are backstops.
Unknown mutation outcomes retain their original sequence for actions_status;
no new mutation is sent while that outcome remains unresolved.

Legacy input tools retain returned-screenshot pixel/frame semantics. Explicit
screenshots return real PNG blocks and matching metadata. By default actions
return their receipt immediately after delivery/cleanup with observationStatus
not-requested, without taking a screenshot. Set observe_after=true to opt in to
a bounded after-image, or observe explicitly. Sent means transport delivery,
not verified application success. Phone data may reach the selected model.

## Release scope

The v1 hand-out supports local stdio. Earlier remote-onboarding notes are kept
in history/mcp-remote-pre-v1.md; they are not current release instructions.

## Selected mobile tools

The v1 selection is mobile_launch_app(packageName), mobile_list_apps,
mobile_list_elements_on_screen, mobile_take_screenshot,
mobile_click_on_screen_at_coordinates(x,y),
mobile_swipe_on_screen(direction,x?,y?,distance?), mobile_type_keys(text,submit?),
and mobile_press_button(button). Optional device is an exact opaque ID; omit
it for one connected phone. BACK/HOME/ENTER use explicitly labeled ADB, not HID.
Other touch/swipe/ASCII remains AOA HID. locale/ref/URL/install/other buttons
and additional gesture aliases are outside this cut.

List elements first, tap a rectangle center, then list again. Rectangles/mobile
coordinates are SCREEN pixels. XML labels are delimited untrusted phone data;
never instructions. Password/editable values are omitted. Native-size mobile
PNGs use that same screen grid, so no scaling is invented. Legacy screenshot
tools retain their own returned-image/frame coordinate contract. Element dumps
prefer stdout; one idle retry is bounded. Fallback uses an operation-owned
random filename instead of a shared cg.xml and attempts cleanup even on caller
cancellation. Disconnect/process crash can prevent remote cleanup; no crash
retention guarantee is claimed. No automatic screenshot persistence is added.

Element collection has a 6.5-second total dump budget, bounded host processes and
phone-side `timeout 3`. After one failed/idle normal dump it retries once with
`--compressed` to an owned temporary file, then cleans that file. Duration/outcome
are logged without XML, serials or labels. If the dump remains unavailable, the
mobile element tool explicitly reports failure and returns a fresh native-size
screenshot through the existing observation path. Failure of that screenshot is
also reported. No element list or application success is invented. A screenshot
fallback is observation requested by this tool, not post-action capture.

Prefer passing the returned element's `element` reference to
`mobile_click_on_screen_at_coordinates`, for example
`{"element":{"frameId":"<current frame>","index":0}}`. The server looks up
its cached rectangle and computes the center; clients supply no rectangle or
scaling math. References expire on new observations, screenshot fallback,
app/button controls, release and session replacement. Stale/forged indices and
mixed element+x/y requests reject. The same daemon frame, routing, ownership,
sequence, bounds and execution-time validation still apply. Explicit x/y inputs
remain SCREEN pixels; legacy screenshot-pixel calls keep their contract.

`mobile_launch_app({"appName":"Settings"})` resolves on the daemon using Android's
framework action, then launches the validated package. Camera has a framework
role too; only Settings has release-phone evidence. Other names require a unique
installed package suffix, such as `browser` matching a package ending `.browser`.
This is not full localized app-label lookup. Unknown/ambiguous names fail; use
`packageName` from `mobile_list_apps` for exact selection. Supply exactly one of
appName/packageName. Resolution is read-only, bounded by the original deadline,
and precedes mutation admission. The original request remains the replay identity;
retained outcomes never re-resolve or replay input. No phone-model package table.
