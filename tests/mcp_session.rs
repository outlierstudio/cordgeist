//! SOFTWARE FAKE daemon through actual official-rmcp stdio; no hardware evidence.
#![cfg(all(target_os = "linux", feature = "fake"))]
use std::{
    io::{BufRead, BufReader, Write},
    os::unix::fs::PermissionsExt,
    process::{Command, Stdio},
};
fn rpc(
    input: &mut impl Write,
    output: &mut impl BufRead,
    id: u64,
    method: &str,
    params: serde_json::Value,
) -> serde_json::Value {
    writeln!(
        input,
        "{}",
        serde_json::json!({"jsonrpc":"2.0","id":id,"method":method,"params":params})
    )
    .unwrap();
    let mut line = String::new();
    output.read_line(&mut line).unwrap();
    let result: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(result["id"], id);
    result
}
#[test]
fn text_only_session_owns_authority_and_hands_back() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let exe = env!("CARGO_BIN_EXE_cordgeist");
    let start = Command::new(exe)
        .arg("--runtime")
        .arg(dir.path())
        .args(["daemon", "start"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert!(start.status.success());
    let mut child = Command::new(exe)
        .arg("--runtime")
        .arg(dir.path())
        .arg("mcp")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    rpc(
        &mut input,
        &mut output,
        1,
        "initialize",
        serde_json::json!({"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"text-only-software-test","version":"1"}}),
    );
    writeln!(
        input,
        "{}",
        serde_json::json!({"jsonrpc":"2.0","method":"notifications/initialized"})
    )
    .unwrap();
    let listed = rpc(
        &mut input,
        &mut output,
        2,
        "tools/list",
        serde_json::json!({}),
    );
    let schemas = listed.to_string();
    assert!(!schemas.contains("credential"));
    for t in listed["result"]["tools"].as_array().unwrap() {
        let name = t["name"].as_str().unwrap();
        assert!(!name.starts_with("leases_") && !name.starts_with("media_"));
        assert!(t["inputSchema"]["properties"].get("auth").is_none());
    }
    let shot = rpc(
        &mut input,
        &mut output,
        3,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{"longest_edge":8}}),
    );
    assert_ne!(shot["result"]["isError"], true);
    assert!(!shot.to_string().contains("credential"));
    assert!(!shot.to_string().contains("leaseId"));
    let text = shot["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|b| b["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let value: serde_json::Value = serde_json::from_str(text).unwrap();
    let frame = value["value"]["frame"]["id"].as_str().unwrap();
    let invalid = rpc(
        &mut input,
        &mut output,
        30,
        "tools/call",
        serde_json::json!({"name":"input_tap","arguments":{"frame_id":"missing-owned-frame","point":{"x":1.0,"y":1.0}}}),
    );
    assert!(invalid.to_string().contains("frame-expired"));
    let tap = rpc(
        &mut input,
        &mut output,
        4,
        "tools/call",
        serde_json::json!({"name":"input_tap","arguments":{"frame_id":frame,"point":{"x":1.0,"y":1.0}}}),
    );
    assert_ne!(tap["result"]["isError"], true);
    assert!(!tap.to_string().contains("credential"));
    let action_text = tap["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|b| b["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let action_value: serde_json::Value = serde_json::from_str(action_text).unwrap();
    assert_eq!(
        action_value["value"]["receipt"]["observationStatus"],
        "not-requested"
    );
    assert!(
        !tap["result"]["content"]
            .as_array()
            .unwrap()
            .iter()
            .any(|b| b["type"] == "image")
    );
    let other = Command::new(exe)
        .arg("--runtime")
        .arg(dir.path())
        .args([
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "second",
            "--lease-file",
        ])
        .arg(dir.path().join("other.json"))
        .output()
        .unwrap();
    assert_eq!(other.status.code(), Some(2));
    let release = rpc(
        &mut input,
        &mut output,
        5,
        "tools/call",
        serde_json::json!({"name":"session_finish","arguments":{}}),
    );
    assert_ne!(release["result"]["isError"], true);
    drop(input);
    child.wait().unwrap();
    let next = Command::new(exe)
        .arg("--runtime")
        .arg(dir.path())
        .args([
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "after",
            "--lease-file",
        ])
        .arg(dir.path().join("after.json"))
        .output()
        .unwrap();
    assert!(next.status.success());
    let next_doc: serde_json::Value =
        serde_json::from_slice(&std::fs::read(dir.path().join("after.json")).unwrap()).unwrap();
    assert_eq!(
        next_doc["auth"]["bootId"], value["value"]["frame"]["provenance"]["bootId"],
        "same daemon boot"
    );
    assert!(
        next_doc["auth"]["fence"].as_u64().unwrap()
            > value["value"]["frame"]["provenance"]["fence"]
                .as_u64()
                .unwrap(),
        "new owner authority fence"
    );
    Command::new(exe)
        .arg("--runtime")
        .arg(dir.path())
        .args(["daemon", "stop"])
        .output()
        .unwrap();
}

fn client(
    runtime: &std::path::Path,
) -> (
    std::process::Child,
    std::process::ChildStdin,
    BufReader<std::process::ChildStdout>,
) {
    let mut p = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .arg("mcp")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = p.stdin.take().unwrap();
    let mut output = BufReader::new(p.stdout.take().unwrap());
    rpc(
        &mut input,
        &mut output,
        1,
        "initialize",
        serde_json::json!({"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"software-lifecycle","version":"1"}}),
    );
    writeln!(
        input,
        "{}",
        serde_json::json!({"jsonrpc":"2.0","method":"notifications/initialized"})
    )
    .unwrap();
    (p, input, output)
}
fn fake_daemon(runtime: &std::path::Path, delay: Option<&str>) {
    std::fs::set_permissions(runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let mut c = Command::new(env!("CARGO_BIN_EXE_cordgeist"));
    c.arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only");
    if let Some(delay) = delay {
        c.env("CORDGEIST_SOFTWARE_DELAY_MS", delay);
    }
    c.env("CORDGEIST_EFFECT_LOG", runtime.join("effects"));
    assert!(c.output().unwrap().status.success());
}
fn stop(runtime: &std::path::Path) {
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "stop"])
        .output()
        .unwrap();
}
fn acquire_after(runtime: &std::path::Path) {
    let p = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args([
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "next-owner",
            "--lease-file",
        ])
        .arg(runtime.join("next.json"))
        .output()
        .unwrap();
    assert!(p.status.success());
}
#[test]
fn concurrent_first_calls_reuse_one_session_authority_and_orderly_exit_releases() {
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), None);
    let (mut child, mut input, mut output) = client(dir.path());
    for id in [2, 3] {
        writeln!(input,"{}",serde_json::json!({"jsonrpc":"2.0","id":id,"method":"tools/call","params":{"name":"observations_screenshot","arguments":{"longest_edge":8}}})).unwrap();
    }
    input.flush().unwrap();
    let mut frames = vec![];
    let mut frame_id = String::new();
    for _ in 0..2 {
        let mut line = String::new();
        output.read_line(&mut line).unwrap();
        let j: serde_json::Value = serde_json::from_str(&line).unwrap();
        assert_ne!(j["result"]["isError"], true);
        assert!(!line.contains("credential"));
        let text = j["result"]["content"]
            .as_array()
            .unwrap()
            .iter()
            .find(|v| v["type"] == "text")
            .unwrap()["text"]
            .as_str()
            .unwrap();
        let v: serde_json::Value = serde_json::from_str(text).unwrap();
        frame_id = v["value"]["frame"]["id"].as_str().unwrap().to_owned();
        frames.push(v["value"]["frame"]["provenance"].clone());
    }
    assert_eq!(frames[0]["fence"], frames[1]["fence"]);
    assert_eq!(frames[0]["attachment"], frames[1]["attachment"]);
    for id in [4, 5] {
        writeln!(input,"{}",serde_json::json!({"jsonrpc":"2.0","id":id,"method":"tools/call","params":{"name":"input_tap","arguments":{"frame_id":frame_id,"point":{"x":1.0,"y":1.0}}}})).unwrap();
    }
    input.flush().unwrap();
    let mut sequences = vec![];
    for _ in 0..2 {
        let mut line = String::new();
        output.read_line(&mut line).unwrap();
        let j: serde_json::Value = serde_json::from_str(&line).unwrap();
        assert_ne!(j["result"]["isError"], true);
        let text = j["result"]["content"]
            .as_array()
            .unwrap()
            .iter()
            .find(|v| v["type"] == "text")
            .unwrap()["text"]
            .as_str()
            .unwrap();
        let v: serde_json::Value = serde_json::from_str(text).unwrap();
        sequences.push(v["value"]["receipt"]["seq"].as_u64().unwrap());
    }
    sequences.sort();
    assert_eq!(sequences, vec![1, 2]);
    assert_eq!(
        std::fs::read_to_string(dir.path().join("effects"))
            .unwrap()
            .lines()
            .count(),
        2
    );
    let (mut second, mut in2, mut out2) = client(dir.path());
    let busy = rpc(
        &mut in2,
        &mut out2,
        2,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{}}),
    );
    assert!(busy.to_string().contains("leased"));
    drop(input);
    child.wait().unwrap();
    let next = rpc(
        &mut in2,
        &mut out2,
        3,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{"longest_edge":8}}),
    );
    assert_ne!(next["result"]["isError"], true);
    rpc(
        &mut in2,
        &mut out2,
        4,
        "tools/call",
        serde_json::json!({"name":"session_finish","arguments":{}}),
    );
    drop(in2);
    second.wait().unwrap();
    stop(dir.path());
}
#[test]
fn uncertain_mutation_never_allocates_a_new_effect() {
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), Some("1700"));
    let (mut child, mut input, mut output) = client(dir.path());
    let shot = rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{"longest_edge":8}}),
    );
    let text = shot["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|v| v["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let v: serde_json::Value = serde_json::from_str(text).unwrap();
    let frame = v["value"]["frame"]["id"].as_str().unwrap();
    let initial = rpc(
        &mut input,
        &mut output,
        3,
        "tools/call",
        serde_json::json!({"name":"input_tap","arguments":{"frame_id":frame,"point":{"x":1.0,"y":1.0}}}),
    );
    let initial_text = initial["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let initial_value: serde_json::Value = serde_json::from_str(initial_text).unwrap();
    assert_eq!(initial_value["method"], "actions.status");
    assert_eq!(initial_value["value"]["action"]["receipt"]["seq"], 1);
    assert_eq!(
        initial_value["value"]["action"]["receipt"]["inputStatus"],
        "unknown"
    );
    assert!(initial_text.contains("process-timeout"));
    assert!(initial_text.contains("stage="));
    assert!(initial_text.contains("deadline="));
    assert!(!initial_text.contains("credential"));
    std::thread::sleep(std::time::Duration::from_millis(500));
    let resolved = rpc(
        &mut input,
        &mut output,
        4,
        "tools/call",
        serde_json::json!({"name":"input_tap","arguments":{"frame_id":frame,"point":{"x":2.0,"y":2.0}}}),
    );
    let text = resolved["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|v| v["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let v: serde_json::Value = serde_json::from_str(text).unwrap();
    assert_eq!(v["method"], "actions.status");
    assert_eq!(v["value"]["action"]["receipt"]["seq"], 1);
    assert_eq!(
        std::fs::read_to_string(dir.path().join("effects"))
            .unwrap()
            .lines()
            .count(),
        1
    );
    drop(input);
    child.wait().unwrap();
    stop(dir.path());
}
#[test]
fn idle_connection_releases_without_client_termination() {
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), None);
    let (mut child, mut input, mut output) = client(dir.path());
    rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{"longest_edge":8}}),
    );
    std::thread::sleep(std::time::Duration::from_secs(47));
    acquire_after(dir.path());
    drop(input);
    child.wait().unwrap();
    stop(dir.path());
}
#[test]
fn killed_client_expires_in_daemon_without_async_drop() {
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), None);
    let (mut child, mut input, mut output) = client(dir.path());
    rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"observations_screenshot","arguments":{"longest_edge":8}}),
    );
    child.kill().unwrap();
    child.wait().unwrap();
    drop(input);
    std::thread::sleep(std::time::Duration::from_secs(62));
    acquire_after(dir.path());
    stop(dir.path());
}

fn text_result(reply: &serde_json::Value) -> serde_json::Value {
    assert_ne!(reply["result"]["isError"], true);
    let text = reply["result"]["content"]
        .as_array()
        .unwrap()
        .iter()
        .find(|b| b["type"] == "text")
        .unwrap()["text"]
        .as_str()
        .unwrap();
    let text = if text.starts_with("BEGIN UNTRUSTED PHONE DATA") {
        text.split_once('\n')
            .unwrap()
            .1
            .rsplit_once("\nEND UNTRUSTED PHONE DATA")
            .unwrap()
            .0
    } else {
        text
    };
    serde_json::from_str(text).unwrap()
}
#[test]
fn text_only_mobile_workflow_uses_readable_labels_and_never_authority() {
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), None);
    let (mut child, mut input, mut output) = client(dir.path());
    let list = rpc(
        &mut input,
        &mut output,
        2,
        "tools/list",
        serde_json::json!({}),
    );
    assert_eq!(
        list["result"]["tools"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|t| t["name"].as_str().unwrap().starts_with("mobile_"))
            .count(),
        8
    );
    for t in list["result"]["tools"].as_array().unwrap() {
        let schema = t["inputSchema"].to_string();
        assert!(!schema.contains("credential") && !schema.contains("\"auth\""));
    }
    let apps = rpc(
        &mut input,
        &mut output,
        3,
        "tools/call",
        serde_json::json!({"name":"mobile_list_apps","arguments":{}}),
    );
    assert_eq!(
        text_result(&apps)["value"]["apps"][0]["packageName"],
        "com.android.settings"
    );
    let launch = rpc(
        &mut input,
        &mut output,
        4,
        "tools/call",
        serde_json::json!({"name":"mobile_launch_app","arguments":{"appName":"Settings"}}),
    );
    assert_eq!(text_result(&launch)["via"], "software-fake");
    let elements = rpc(
        &mut input,
        &mut output,
        5,
        "tools/call",
        serde_json::json!({"name":"mobile_list_elements_on_screen","arguments":{}}),
    );
    assert!(!elements.to_string().contains("SYNTHETIC_SECRET"));
    let v = text_result(&elements);
    let search = v["value"]["elements"]
        .as_array()
        .unwrap()
        .iter()
        .find(|e| e["label"] == "Search")
        .unwrap();
    let reference = search["element"].clone();
    assert!(reference["frameId"].is_string());
    let tap = rpc(
        &mut input,
        &mut output,
        6,
        "tools/call",
        serde_json::json!({"name":"mobile_click_on_screen_at_coordinates","arguments":{"element":reference}}),
    );
    assert_eq!(
        text_result(&tap)["value"]["receipt"]["observationStatus"],
        "not-requested"
    );
    rpc(
        &mut input,
        &mut output,
        7,
        "tools/call",
        serde_json::json!({"name":"mobile_list_elements_on_screen","arguments":{}}),
    );
    let stale = rpc(
        &mut input,
        &mut output,
        71,
        "tools/call",
        serde_json::json!({"name":"mobile_click_on_screen_at_coordinates","arguments":{"element":reference}}),
    );
    assert!(stale.get("error").is_some() || stale["result"]["isError"] == true);
    let typed = rpc(
        &mut input,
        &mut output,
        8,
        "tools/call",
        serde_json::json!({"name":"mobile_type_keys","arguments":{"text":"SYNTHETIC_QUERY"}}),
    );
    assert!(!typed.to_string().contains("SYNTHETIC_QUERY"));
    assert!(!typed.to_string().contains("credential"));
    let after = rpc(
        &mut input,
        &mut output,
        9,
        "tools/call",
        serde_json::json!({"name":"mobile_list_elements_on_screen","arguments":{}}),
    );
    let v = text_result(&after);
    let e = v["value"]["elements"].as_array().unwrap();
    assert!(e.iter().any(|e| e["text"] == "Bluetooth"));
    assert!(e.iter().any(|e| e["text"] == "Off"));
    let shot = rpc(
        &mut input,
        &mut output,
        10,
        "tools/call",
        serde_json::json!({"name":"mobile_take_screenshot","arguments":{}}),
    );
    let v = text_result(&shot);
    assert_eq!(v["value"]["frame"]["image"], v["value"]["frame"]["display"]);
    rpc(
        &mut input,
        &mut output,
        11,
        "tools/call",
        serde_json::json!({"name":"session_finish","arguments":{}}),
    );
    drop(input);
    child.wait().unwrap();
    stop(dir.path());
}

#[test]
fn unavailable_elements_return_fresh_screenshot_and_explicit_error() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    assert!(
        Command::new(env!("CARGO_BIN_EXE_cordgeist"))
            .arg("--runtime")
            .arg(dir.path())
            .args(["daemon", "start"])
            .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
            .env("CORDGEIST_SOFTWARE_UI_UNAVAILABLE", "1")
            .output()
            .unwrap()
            .status
            .success()
    );
    let (mut child, mut input, mut output) = client(dir.path());
    let reply = rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"mobile_list_elements_on_screen","arguments":{}}),
    );
    let content = reply["result"]["content"].as_array().unwrap();
    assert!(content.iter().any(|c| c["type"] == "image"));
    assert!(content.iter().any(|c| {
        c["text"]
            .as_str()
            .is_some_and(|s| s.contains("Element dump unavailable"))
    }));
    assert!(!reply.to_string().contains("credential"));
    let tap = rpc(
        &mut input,
        &mut output,
        3,
        "tools/call",
        serde_json::json!({"name":"mobile_click_on_screen_at_coordinates","arguments":{"x":1,"y":1}}),
    );
    assert!(!tap["result"]["isError"].as_bool().unwrap_or(false));
    drop(input);
    child.wait().unwrap();
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(dir.path())
        .args(["daemon", "stop"])
        .output()
        .unwrap();
}

#[test]
fn elements_over_two_seconds_use_observation_deadline_in_production_ipc() {
    // SOFTWARE evidence: actual rmcp -> IPC server -> ServiceDispatcher.
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    assert!(
        Command::new(env!("CARGO_BIN_EXE_cordgeist"))
            .arg("--runtime")
            .arg(dir.path())
            .args(["daemon", "start"])
            .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
            .env("CORDGEIST_SOFTWARE_UI_UNAVAILABLE", "slow")
            .output()
            .unwrap()
            .status
            .success()
    );
    let (mut child, mut input, mut output) = client(dir.path());
    let start = std::time::Instant::now();
    let reply = rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"mobile_list_elements_on_screen","arguments":{}}),
    );
    assert!(start.elapsed() >= std::time::Duration::from_millis(2200));
    assert!(start.elapsed() < std::time::Duration::from_secs(8));
    assert!(
        !text_result(&reply)["value"]["elements"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert!(
        !reply["result"]["content"]
            .as_array()
            .unwrap()
            .iter()
            .any(|c| c["type"] == "image"),
        "must collect elements, not silently substitute screenshot"
    );
    drop(input);
    child.wait().unwrap();
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(dir.path())
        .args(["daemon", "stop"])
        .output()
        .unwrap();
}

#[test]
fn app_launch_over_two_seconds_passes_through_official_rmcp_and_dispatcher() {
    // SOFTWARE ONLY: delay the actual owner submission, not the protocol reply.
    let dir = tempfile::tempdir().unwrap();
    fake_daemon(dir.path(), Some("2200"));
    let (mut child, mut input, mut output) = client(dir.path());
    let rejected = rpc(
        &mut input,
        &mut output,
        2,
        "tools/call",
        serde_json::json!({"name":"mobile_launch_app","arguments":{"appName":"Unknown;Name"}}),
    );
    assert_eq!(rejected["error"]["code"], -32602);
    assert!(rejected.to_string().contains("app-name-resolution"));
    assert!(rejected.to_string().contains("No launch admitted"));
    let started = std::time::Instant::now();
    let response = rpc(
        &mut input,
        &mut output,
        3,
        "tools/call",
        serde_json::json!({"name":"mobile_launch_app","arguments":{"appName":"Chrome"}}),
    );
    let value = text_result(&response);
    assert!(started.elapsed() >= std::time::Duration::from_secs(2));
    assert_eq!(value["value"]["receipt"]["inputStatus"], "sent");
    assert_eq!(value["value"]["receipt"]["seq"], 1);
    assert_eq!(value["via"], "software-fake");
    let finish = rpc(
        &mut input,
        &mut output,
        4,
        "tools/call",
        serde_json::json!({"name":"session_finish","arguments":{}}),
    );
    assert_ne!(finish["result"]["isError"], true);
    drop(input);
    child.wait().unwrap();
    stop(dir.path());
}
