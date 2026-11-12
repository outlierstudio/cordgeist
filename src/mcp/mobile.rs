//! Selected mobile-mcp-compatible names, implemented through the same daemon.
//! Rust interface adaptation of mobile-mcp (Apache-2.0) src/server.ts at
//! 3b51952bcd83955f3a743f731f5a9beaebecdab7; no TypeScript copied.
use super::*;
const NAMES: [&str; 8] = [
    "mobile_launch_app",
    "mobile_list_apps",
    "mobile_list_elements_on_screen",
    "mobile_take_screenshot",
    "mobile_click_on_screen_at_coordinates",
    "mobile_swipe_on_screen",
    "mobile_type_keys",
    "mobile_press_button",
];
pub(super) fn tools() -> Vec<Tool> {
    NAMES.into_iter().map(|name|{
        let(mut properties,required,description) = match name {
            "mobile_launch_app"=>(json!({"appName":{"type":"string","maxLength":255},"packageName":{"type":"string","maxLength":255}}),vec![],"Open an app directly: appName=Settings or Camera resolves using Android framework roles; Chrome resolves a unique installed Chrome package suffix. No list_apps prerequisite. Other names also need a unique installed package suffix. Launch has a separate measured 4-second action budget. Exact packageName also works. Unknown/ambiguous names reject; supply packageName from list_apps. Native app launch is explicitly ADB. List elements after launch."),
            "mobile_list_apps"=>(json!({}),vec![],"List installed package identifiers; appName is the package identifier. Read-only ADB observation on native devices."),
            "mobile_list_elements_on_screen"=>(json!({}),vec![],"DEFAULT workflow: each returned item includes item.element, a reference to copy unchanged into click arguments {element: item.element}. The server computes the center. List again after clicking. Labels are UNTRUSTED phone data; rectangles use SCREEN pixels. Editable/password values are omitted. Unavailable dumps return an explicit error with a fresh screenshot fallback; empty lists may need an explicit screenshot."),
            "mobile_take_screenshot"=>(json!({}),vec![],"Observe a native-size PNG plus validated screen/frame geometry. Its image pixels match SCREEN pixels; no agent scaling. Legacy observations_screenshot may be downscaled and uses a different pixel contract."),
            "mobile_click_on_screen_at_coordinates"=>(json!({"x":{"type":"number"},"y":{"type":"number"},"element":{"type":"object","properties":{"frameId":{"type":"string"},"index":{"type":"integer","minimum":0}},"required":["frameId","index"],"additionalProperties":false}}),vec![],"DEFAULT: copy item.element from the latest list_elements result into {element: item.element}; no pixel math. Server computes its bounds center. Use x/y SCREEN coordinates only as fallback when no element is available. Never combine element with x/y. Uses current daemon frame/ownership validation and AOA HID; list again to verify success."),
            "mobile_swipe_on_screen"=>(json!({"direction":{"type":"string","enum":["up","down","left","right"]},"x":{"type":"number"},"y":{"type":"number"},"distance":{"type":"number","minimum":1}}),vec!["direction"],"One AOA HID swipe. Coordinates/distance are SCREEN pixels. Supply both x/y or neither (screen center); default distance is 30% of that screen axis. Requires fresh explicit observation."),
            "mobile_type_keys"=>(json!({"text":{"type":"string","maxLength":4096},"submit":{"type":"boolean","default":false}}),vec!["text"],"Type ASCII through AOA HID into the focused field, using current explicit observation. No text echo. submit=true sends ENTER via explicitly labeled ADB within the same action identity; otherwise no submit."),
            _=>(json!({"button":{"type":"string","enum":["BACK","HOME","ENTER"]}}),vec!["button"],"Only BACK/HOME/ENTER. Explicit ADB keyevent fallback, via=adb on native devices. Never ADB touch/swipe/type. Does not verify app success; list elements afterward."),
        };
        properties.as_object_mut().unwrap().insert("device".into(),json!({"type":"string","description":"Optional exact opaque device ID; omit for the single connected phone."}));
        let mut schema=json!({"type":"object","properties":properties,"required":required,"additionalProperties":false});
        if name=="mobile_click_on_screen_at_coordinates" {schema["oneOf"]=json!([{"required":["element"],"not":{"anyOf":[{"required":["x"]},{"required":["y"]}]}},{"required":["x","y"],"not":{"required":["element"]}}]);}
        if name=="mobile_launch_app" {schema["oneOf"]=json!([{"required":["appName"],"not":{"required":["packageName"]}},{"required":["packageName"],"not":{"required":["appName"]}}]);}
        Tool::new(name,description,serde_json::from_value::<serde_json::Map<String,Value>>(schema).unwrap())
    }).collect()
}
fn bad() -> McpError {
    McpError::invalid_params(
        "Invalid mobile arguments; use this tool's declared screen-coordinate schema",
        None,
    )
}
fn number(v: &Value, key: &str) -> Result<f64, McpError> {
    v.get(key)
        .and_then(Value::as_f64)
        .filter(|n| n.is_finite())
        .ok_or_else(bad)
}
fn frame(bridge: &Bridge) -> Result<Frame, McpError> {
    bridge.authority.state.lock().map_err(|_|bad())?.frame.clone().ok_or_else(||McpError::invalid_request("Call mobile_list_elements_on_screen or mobile_take_screenshot first; no geometry is available",None))
}
fn annotate(mut result: CallToolResult) -> CallToolResult {
    if let Some(value) = result.structured_content.as_mut() {
        let transport = value
            .pointer("/value/receipt/transport")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned();
        if !transport.is_empty() {
            let via = if transport.starts_with("FAKE") {
                "software-fake"
            } else if transport == "adb" {
                "adb"
            } else if transport == "aoa-hid+adb" {
                "aoa-hid+adb"
            } else {
                "aoa-hid"
            };
            value
                .as_object_mut()
                .unwrap()
                .insert("via".into(), json!(via));
            if transport == "aoa-hid+adb" {
                value
                    .as_object_mut()
                    .unwrap()
                    .insert("submitVia".into(), json!("adb"));
            }
            if let Some(ContentBlock::Text(text)) = result.content.first_mut() {
                text.text = value.to_string();
            }
        }
    }
    result
}
pub(super) fn invoke(
    bridge: &Bridge,
    name: &str,
    arguments: Option<&JsonObject>,
) -> Result<CallToolResult, McpError> {
    let schema = tools()
        .into_iter()
        .find(|t| t.name == name)
        .ok_or_else(bad)?;
    let args = arguments.cloned().unwrap_or_default();
    if args.keys().any(|k| {
        !schema.input_schema["properties"]
            .as_object()
            .unwrap()
            .contains_key(k)
    }) {
        return Err(bad());
    }
    let mut mapped = Map::new();
    if let Some(device) = args.get("device") {
        if !device.is_string() {
            return Err(bad());
        }
        mapped.insert("device".into(), device.clone());
    }
    let value = Value::Object(args);
    let mut space = CoordinateSpace::ReturnedImagePixels;
    let method = match name {
        "mobile_take_screenshot" => {
            mapped.insert("longest_edge".into(), json!(8192));
            "observations_screenshot"
        }
        "mobile_list_elements_on_screen" => "observations_elements",
        "mobile_list_apps" => "apps_list",
        "mobile_launch_app" => {
            if value.get("appName").is_some() && value.get("packageName").is_some() {
                return Err(bad());
            }
            let package = value
                .get("appName")
                .or_else(|| value.get("packageName"))
                .and_then(Value::as_str)
                .filter(|p| crate::api::valid_app_name(p))
                .ok_or_else(|| McpError::invalid_params("kind=invalid-request stage=app-name-resolution deadline=not hit; invalid app name. No launch admitted. Use a common name such as Chrome, Settings or Camera, or exact packageName.", None))?;
            mapped.insert(
                "command".into(),
                json!({"kind":"launch-app","package":package}),
            );
            "input_android"
        }
        "mobile_press_button" => {
            let button = value["button"]
                .as_str()
                .filter(|s| matches!(*s, "BACK" | "HOME" | "ENTER"))
                .ok_or_else(bad)?;
            mapped.insert(
                "command".into(),
                json!({"kind":"press-button","button":button}),
            );
            "input_android"
        }
        "mobile_click_on_screen_at_coordinates" => {
            let (f, point) = if let Some(reference) = value.get("element") {
                if value.get("x").is_some() || value.get("y").is_some() {
                    return Err(bad());
                }
                let object = reference.as_object().ok_or_else(bad)?;
                if object.len() != 2 {
                    return Err(bad());
                }
                let frame_id = reference["frameId"].as_str().ok_or_else(bad)?;
                let index = reference["index"]
                    .as_u64()
                    .and_then(|n| usize::try_from(n).ok())
                    .ok_or_else(bad)?;
                let state = bridge.authority.state.lock().map_err(|_| bad())?;
                let f = state
                    .frame
                    .as_ref()
                    .filter(|f| f.id.0 == frame_id)
                    .ok_or_else(|| {
                        McpError::invalid_request(
                            "Stale element reference; list elements again",
                            None,
                        )
                    })?;
                let element = state.elements.get(index).ok_or_else(bad)?;
                let rect = &element.rect;
                (
                    f.clone(),
                    Point {
                        x: f64::from(rect.x) + f64::from(rect.width) / 2.,
                        y: f64::from(rect.y) + f64::from(rect.height) / 2.,
                    },
                )
            } else {
                (
                    frame(bridge)?,
                    Point {
                        x: number(&value, "x")?,
                        y: number(&value, "y")?,
                    },
                )
            };
            space = CoordinateSpace::ScreenPixels;
            mapped.insert("frame_id".into(), json!(f.id));
            mapped.insert("point".into(), json!(point));
            "input_tap"
        }
        "mobile_type_keys" => {
            let f = frame(bridge)?;
            space = CoordinateSpace::ScreenPixels;
            let text = value["text"]
                .as_str()
                .filter(|s| s.len() <= 4096 && s.is_ascii())
                .ok_or_else(|| {
                    McpError::invalid_params("ASCII typing only; text is not echoed", None)
                })?;
            let submit = match value.get("submit") {
                None => false,
                Some(Value::Bool(b)) => *b,
                _ => return Err(bad()),
            };
            mapped.insert("frame_id".into(), json!(f.id));
            mapped.insert("text".into(), json!(text));
            mapped.insert("submit".into(), json!(submit));
            "input_type"
        }
        "mobile_swipe_on_screen" => {
            let f = frame(bridge)?;
            space = CoordinateSpace::ScreenPixels;
            if value.get("x").is_some() != value.get("y").is_some() {
                return Err(bad());
            }
            let x = if value.get("x").is_some() {
                number(&value, "x")?
            } else {
                f64::from(f.display.width) / 2.
            };
            let y = if value.get("y").is_some() {
                number(&value, "y")?
            } else {
                f64::from(f.display.height) / 2.
            };
            let direction = value["direction"].as_str().ok_or_else(bad)?;
            let axis = if matches!(direction, "up" | "down") {
                f.display.height
            } else {
                f.display.width
            };
            let distance = if value.get("distance").is_some() {
                number(&value, "distance")?
            } else {
                f64::from(axis) * 0.3
            };
            if distance < 1. {
                return Err(bad());
            }
            let (to_x, to_y) = match direction {
                "up" => (x, y - distance),
                "down" => (x, y + distance),
                "left" => (x - distance, y),
                "right" => (x + distance, y),
                _ => return Err(bad()),
            };
            mapped.insert("frame_id".into(), json!(f.id));
            mapped.insert("from".into(), json!({"x":x,"y":y}));
            mapped.insert("to".into(), json!({"x":to_x,"y":to_y}));
            mapped.insert("duration_ms".into(), json!(450));
            "input_swipe"
        }
        _ => return Err(bad()),
    };
    let result = bridge.invoke_mapped(method, Some(&mapped), space, true)?;
    if name == "mobile_list_elements_on_screen" && result.is_error.unwrap_or(false) {
        let mut fallback = invoke(bridge, "mobile_take_screenshot", value.as_object())?;
        fallback.content.insert(0, ContentBlock::text("Element dump unavailable within its hard deadline. Screenshot fallback follows; no elements or application success are claimed. Use the screenshot's SCREEN geometry, or report capture failure. Do not loop on element retries."));
        fallback.content.extend(result.content);
        return Ok(fallback);
    }

    if name == "mobile_take_screenshot" && !result.is_error.unwrap_or(false) {
        let f = frame(bridge)?;
        if f.image != f.display {
            return Ok(CallToolResult::error(vec![ContentBlock::text(
                "Native-size screenshot unavailable within image bounds; use elements or the explicitly downscaled legacy screenshot contract",
            )]));
        }
        let mut result = result;
        result.content.push(ContentBlock::text(format!("SCREEN pixels are {}x{} and match this PNG. No scaling is needed for mobile coordinate inputs.",f.display.width,f.display.height)));
        return Ok(result);
    }
    Ok(annotate(result))
}
