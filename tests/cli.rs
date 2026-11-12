use std::process::Command;
mod common;
#[test]
fn doctor_reports_actual_unavailability_and_redacts_inventory() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("fake adb ; diagnostic");
    common::python(
        &path,
        r#"#!/usr/bin/python3
import sys
import os
assert 'ANDROID_SERIAL' not in os.environ
assert 'ADB_SERVER_SOCKET' not in os.environ
args=sys.argv[1:]
assert args[:4]==['-H','127.0.0.1','-P','5039']
if args[4:]==['version']:print('Android Debug Bridge version 1.0.41')
elif args[4:]==['devices','-l']:print('List of devices attached\nPRIVATE_SYNTHETIC_SERIAL unauthorized usb:1-2 transport_id:1\n')
else:raise RuntimeError('unexpected command')
"#,
    );
    let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .env("ANDROID_SERIAL", "wrong-device")
        .env("ADB_SERVER_SOCKET", "tcp:hostile.example:1234")
        .args(["doctor", "--json", "--adb"])
        .arg(path)
        .args(["--adb-port", "5039"])
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(2));
    assert!(out.stderr.is_empty());
    let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(json["backend"], "unavailable");
    assert_eq!(json["hardwareVerified"], false);
    assert_eq!(json["adbDetected"], true);
    let text = String::from_utf8(out.stdout).unwrap();
    assert!(text.contains("adb-unauthorized"));
    assert!(!text.contains("PRIVATE_SYNTHETIC_SERIAL"));
}
#[test]
fn status_and_missing_executable_are_honest_machine_results() {
    for args in [
        vec!["status", "--json"],
        vec!["doctor", "--json", "--adb", "/definitely/missing/adb"],
    ] {
        let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
            .args(args)
            .output()
            .unwrap();
        assert_eq!(out.status.code(), Some(2));
        let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
        assert_eq!(json["daemonImplemented"], true);
        assert_eq!(json["backend"], "unavailable");
    }
}

#[test]
fn doctor_nine_read_only_checks_withhold_phone_data() {
    // Software adapter evidence only. Native USB availability is not mocked into a pass.
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("fake-adb");
    std::fs::write(
        dir.path().join("viewport"),
        include_bytes!("fixtures/aosp-viewport.txt"),
    )
    .unwrap();
    std::fs::write(
        dir.path().join("screen.png"),
        include_bytes!("fixtures/synthetic.png"),
    )
    .unwrap();
    common::python(
        &path,
        r#"#!/usr/bin/python3
import sys,pathlib
base=pathlib.Path(__file__).parent
args=sys.argv[1:]
assert args[:4]==['-H','127.0.0.1','-P','5039']
a=args[4:]
if a==['version']:print('Android Debug Bridge version 1.0.41\nInstalled as PRIVATE_HOST_PATH')
elif a==['devices','-l']:print('List of devices attached\nPRIVATE_PHONE_SERIAL device usb:1-2 transport_id:77\n')
else:
 assert a[:2]==['-t','77']
 a=a[2:]
 if a==['shell','getprop','ro.build.version.release']:print('10')
 elif a==['shell','getprop','ro.product.model']:print('Synthetic Phone')
 elif a==['shell','getprop','ro.product.manufacturer']:print('Test')
 elif a==['shell','wm','size']:print('Physical size: 4x8')
 elif a==['shell','am','get-current-user']:print(0)
 elif a==['shell','dumpsys','input']:print((base/'viewport').read_text())
 elif a==['exec-out','screencap','-p']:sys.stdout.buffer.write((base/'screen.png').read_bytes())
 elif a==['exec-out','timeout','3','uiautomator','dump','/dev/tty']:print('<hierarchy><node class="TextView" text="PRIVATE_UI_TEXT" content-desc="" resource-id="id/test" bounds="[0,0][4,8]" /></hierarchy>')
 else:raise RuntimeError('not a permitted read-only command')
"#,
    );
    let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .args(["doctor", "--json", "--adb"])
        .arg(&path)
        .args(["--adb-port", "5039"])
        .output()
        .unwrap();
    let timing = String::from_utf8_lossy(&out.stderr);
    assert!(
        timing
            .lines()
            .all(|l| l.starts_with("cordgeist ui_dump duration_ms=") && l.ends_with("outcome=ok"))
    );
    assert!(!timing.contains("PRIVATE"));
    let value: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    let checks = value["checks"].as_array().unwrap();
    assert_eq!(checks.len(), 9);
    for (i, c) in checks.iter().enumerate() {
        assert_eq!(c["number"], i + 1);
    }
    for c in &checks[4..] {
        assert_eq!(c["status"], "pass");
    }
    let text = String::from_utf8(out.stdout).unwrap();
    for private in [
        "PRIVATE_PHONE_SERIAL",
        "PRIVATE_UI_TEXT",
        "PRIVATE_HOST_PATH",
        "base64",
    ] {
        assert!(!text.contains(private));
    }
    assert_eq!(value["hardwareVerified"], false);
}
