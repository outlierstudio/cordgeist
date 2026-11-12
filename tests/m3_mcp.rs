//! Official stdio MCP and the authenticated loopback bridge.
#![cfg(all(target_os = "linux", feature = "fake"))]
use rmcp::ServiceExt;
use std::{
    io::{BufRead, BufReader, Write},
    os::unix::fs::PermissionsExt,
    process::{Command, Stdio},
    time::{Duration, Instant},
};
fn dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
fn daemon(runtime: &std::path::Path, effect_log: Option<&std::path::Path>) {
    let mut command = Command::new(env!("CARGO_BIN_EXE_cordgeist"));
    command
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only");
    if let Some(log) = effect_log {
        command.env("CORDGEIST_EFFECT_LOG", log);
    }
    let out = command.output().unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stdout)
    );
}
fn line(
    input: &mut impl Write,
    output: &mut impl BufRead,
    message: serde_json::Value,
) -> serde_json::Value {
    writeln!(input, "{message}").unwrap();
    let mut text = String::new();
    let end = Instant::now() + Duration::from_secs(3);
    while Instant::now() < end {
        text.clear();
        output.read_line(&mut text).unwrap();
        if text.contains("\"jsonrpc\"") {
            return serde_json::from_str(&text).unwrap();
        }
    }
    panic!("no json-rpc reply: {text}");
}
#[test]
fn stdio_lists_catalogue_and_returns_a_png_block() {
    let dir = dir();
    let runtime = dir.path();
    let effects = runtime.join("effects.log");
    daemon(runtime, Some(&effects));
    let mut child = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .arg("mcp")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    let mut output = BufReader::new(child.stdout.take().unwrap());
    let init = line(
        &mut input,
        &mut output,
        serde_json::json!({
            "jsonrpc":"2.0","id":1,"method":"initialize",
            "params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"m3","version":"0"}}
        }),
    );
    assert!(init.get("result").is_some(), "{init}");
    writeln!(
        input,
        r#"{{"jsonrpc":"2.0","method":"notifications/initialized"}}"#
    )
    .unwrap();
    let listed = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":2,"method":"tools/list"}),
    );
    let names: Vec<_> = listed["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .map(|tool| tool["name"].as_str().unwrap().to_string())
        .collect();
    assert!(names.contains(&"devices_list".into()));
    assert!(!names.iter().any(|name| name.starts_with("media_")));
    let hidden = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":200,"method":"tools/call","params":{"name":"media_push","arguments":{}}}),
    );
    assert_eq!(hidden["error"]["code"], -32602);
    assert!(names.contains(&"observations_screenshot".into()));
    assert!(
        !names
            .iter()
            .any(|name| name == "leases_revoke" || name == "daemon_stop")
    );
    let devices = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"devices_list","arguments":{}}}),
    );
    assert!(devices.to_string().contains("software-phone"));
    let bad = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"not_a_tool","arguments":{}}}),
    );
    assert_eq!(bad["error"]["code"], -32602);
    assert!(!names.iter().any(|name| name.starts_with("leases_")));
    let shot_tool = tool(&listed, "observations_screenshot");
    assert!(!schema_lists(shot_tool, "auth"));
    let shot = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"observations_screenshot","arguments":only_schema(shot_tool,serde_json::json!({"longest_edge":8,"settle_ms":0}))}}),
    );
    let blocks = shot["result"]["content"].as_array().unwrap();
    assert!(
        blocks
            .iter()
            .any(|b| b["type"] == "image" && b["mimeType"] == "image/png")
    );
    assert!(!shot.to_string().contains("credential"));
    let frame = shot
        .pointer("/result/structuredContent/value/frame/id")
        .and_then(|v| v.as_str())
        .unwrap();
    let tap_tool = tool(&listed, "input_tap");
    for field in ["frame_id", "point"] {
        assert!(schema_lists(tap_tool, field));
    }
    for field in ["auth", "seq"] {
        assert!(!schema_lists(tap_tool, field));
    }
    let args = only_schema(
        tap_tool,
        serde_json::json!({"frame_id":frame,"point":{"x":1.0,"y":1.0}}),
    );
    let tap = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"input_tap","arguments":args}}),
    );
    assert_ne!(tap["result"]["isError"], true);
    assert_eq!(
        std::fs::read_to_string(&effects).unwrap().lines().count(),
        1
    );
    let status = line(
        &mut input,
        &mut output,
        serde_json::json!({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"actions_status","arguments":{}}}),
    );
    assert_ne!(status["result"]["isError"], true);
    assert_eq!(
        std::fs::read_to_string(&effects).unwrap().lines().count(),
        1
    );
    let cli = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "second",
            "--lease-file",
        ])
        .arg(runtime.join("other.json"))
        .output()
        .unwrap();
    assert_eq!(cli.status.code(), Some(2));
    let _ = child.kill();
    let _ = child.wait();
}
#[tokio::test]
async fn streamable_http_client_lists_tools_and_receives_png() {
    let dir = dir();
    let runtime = dir.path();
    let effects = runtime.join("effects.log");
    daemon(runtime, Some(&effects));
    let token_path = runtime.join("token");
    std::fs::write(&token_path, b"test-token-value\n").unwrap();
    std::fs::set_permissions(&token_path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let mut remote = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["mcp", "remote", "--bind", "127.0.0.1:0", "--token-file"])
        .arg(&token_path)
        .stderr(Stdio::piped())
        .stdout(Stdio::null())
        .spawn()
        .unwrap();
    let mut err = BufReader::new(remote.stderr.take().unwrap());
    let mut bound = String::new();
    err.read_line(&mut bound).unwrap();
    let port = bound.trim().rsplit(':').next().unwrap();
    let url = format!("http://127.0.0.1:{port}/mcp");
    let denied = connect(&url, None).await;
    let denied = denied.expect_err("missing token");
    assert!(!denied.contains("devices_list"), "{denied}");
    let wrong = connect(&url, Some("wrong-token-value"))
        .await
        .expect_err("wrong token");
    assert!(!wrong.contains("devices_list"), "{wrong}");
    let lower = connect_scheme(&url, "bearer test-token-value")
        .await
        .expect("bearer");
    let names = tool_names(&lower).await;
    assert!(names.iter().any(|name| name == "devices_list"));
    assert!(!names.iter().any(|name| name == "leases_revoke"));
    drop(lower);
    let client = connect(&url, Some("test-token-value"))
        .await
        .expect("Bearer");
    let tools = tool_names(&client).await;
    assert!(tools.iter().any(|name| name == "observations_screenshot"));
    let shot = client
        .call_tool(
            rmcp::model::CallToolRequestParams::new("observations_screenshot").with_arguments(
                serde_json::json!({"longest_edge": 8, "settle_ms": 0})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .unwrap();
    assert!(shot.content.iter().any(|block| {
        matches!(block, rmcp::model::ContentBlock::Image(image) if image.mime_type == "image/png" && !image.data.is_empty())
    }));
    let frame = shot
        .structured_content
        .as_ref()
        .and_then(|v| v.pointer("/value/frame/id"))
        .and_then(|v| v.as_str())
        .unwrap()
        .to_string();
    let tap_args = serde_json::json!({"frame_id": frame, "point": {"x": 1.0, "y": 1.0}})
        .as_object()
        .unwrap()
        .clone();
    client
        .call_tool(
            rmcp::model::CallToolRequestParams::new("input_tap").with_arguments(tap_args.clone()),
        )
        .await
        .unwrap();
    let once = std::fs::read_to_string(&effects).unwrap().lines().count();
    assert_eq!(once, 1);
    client
        .call_tool(
            rmcp::model::CallToolRequestParams::new("actions_status")
                .with_arguments(serde_json::Map::new()),
        )
        .await
        .unwrap();
    assert_eq!(
        std::fs::read_to_string(&effects).unwrap().lines().count(),
        once
    );
    let cli = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "second",
            "--lease-file",
        ])
        .arg(runtime.join("other.json"))
        .output()
        .unwrap();
    assert_eq!(cli.status.code(), Some(2));
    drop(client);
    let _ = remote.kill();
    let _ = remote.wait();
}
async fn connect(
    url: &str,
    token: Option<&str>,
) -> Result<rmcp::service::RunningService<rmcp::RoleClient, ()>, String> {
    let mut config =
        rmcp::transport::streamable_http_client::StreamableHttpClientTransportConfig::with_uri(url);
    if let Some(token) = token {
        config = config.auth_header(token);
    }
    ().serve(rmcp::transport::StreamableHttpClientTransport::from_config(
        config,
    ))
    .await
    .map_err(|error| error.to_string())
}
async fn connect_scheme(
    url: &str,
    header: &str,
) -> Result<rmcp::service::RunningService<rmcp::RoleClient, ()>, String> {
    let mut config =
        rmcp::transport::streamable_http_client::StreamableHttpClientTransportConfig::with_uri(url);
    config.custom_headers.insert(
        http::header::AUTHORIZATION,
        http::HeaderValue::from_str(header).unwrap(),
    );
    ().serve(rmcp::transport::StreamableHttpClientTransport::from_config(
        config,
    ))
    .await
    .map_err(|error| error.to_string())
}
async fn tool_names(client: &rmcp::service::RunningService<rmcp::RoleClient, ()>) -> Vec<String> {
    client
        .peer()
        .list_all_tools()
        .await
        .unwrap()
        .into_iter()
        .map(|tool| tool.name.to_string())
        .collect()
}
fn schema_lists(tool: &serde_json::Value, field: &str) -> bool {
    tool["inputSchema"]["properties"]
        .as_object()
        .is_some_and(|props| props.contains_key(field))
}
fn tool<'a>(listed: &'a serde_json::Value, name: &str) -> &'a serde_json::Value {
    listed["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .find(|tool| tool["name"] == name)
        .unwrap()
}
fn only_schema(tool: &serde_json::Value, values: serde_json::Value) -> serde_json::Value {
    let allowed = tool["inputSchema"]["properties"].as_object().unwrap();
    let mut out = serde_json::Map::new();
    for (key, value) in values.as_object().unwrap() {
        assert!(
            allowed.contains_key(key),
            "{key} is not in {}",
            tool["inputSchema"]
        );
        out.insert(key.clone(), value.clone());
    }
    serde_json::Value::Object(out)
}
