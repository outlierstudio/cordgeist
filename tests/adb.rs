use cordgeist::{adb::*, api::ErrorCode};
use std::time::Duration;
mod common;
use tokio_util::sync::CancellationToken;
fn fake(script: &str) -> (tempfile::TempDir, Adb) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("fake adb ; executable");
    common::python(&path, &format!("#!/usr/bin/python3\n{script}\n"));
    let adb = Adb::new(path, 5039).unwrap();
    (dir, adb)
}
#[tokio::test]
async fn exact_routing_cleared_environment_and_binary_capture() {
    let (_dir, adb) = fake(
        r#"
import sys,os
assert sys.argv[1:] == ['-H','127.0.0.1','-P','5039','-t','77','exec-out','screencap','-p'],sys.argv
assert not any(k.startswith('ADB_') or k in ['ANDROID_SERIAL','ANDROID_ADB_SERVER_PORT'] for k in os.environ)
sys.stdout.buffer.write(bytes([0,13,10,255,128]))
"#,
    );
    let out = adb
        .screenshot(&Target { transport_id: 77 }, CancellationToken::new())
        .await
        .unwrap();
    assert_eq!(out.stdout, [0, 13, 10, 255, 128]);
}
#[tokio::test]
async fn deadline_cancellation_output_cap_and_stderr_drain() {
    let (_dir, adb) = fake("import time\ntime.sleep(30)");
    let actual = adb
        .inventory(Duration::from_millis(80), CancellationToken::new())
        .await
        .err();
    eprintln!("deadline/cancellation safe ErrorCode: {actual:?}");
    assert_eq!(actual, Some(ErrorCode::ProcessTimeout));
    let token = CancellationToken::new();
    token.cancel();
    assert!(matches!(
        adb.inventory(Duration::from_secs(1), token).await,
        Err(ErrorCode::ProcessCancelled)
    ));
    let (_dir, adb) =
        fake("import sys\nsys.stdout.buffer.write(b'x'*(1024*1024+1))\nsys.stdout.flush()");
    assert!(matches!(
        adb.inventory(Duration::from_secs(2), CancellationToken::new())
            .await,
        Err(ErrorCode::OutputLimit)
    ));
    let (_dir, adb) = fake(
        "import sys\nsys.stderr.buffer.write(b'x'*(1024*1024))\nsys.stdout.buffer.write(b'bounded')",
    );
    let out = adb
        .inventory(Duration::from_secs(2), CancellationToken::new())
        .await
        .unwrap();
    assert_eq!(out.stdout, b"bounded");
    assert_eq!(out.stderr.len(), 256 * 1024);
    assert!(out.stderr_truncated);
}
#[tokio::test]
async fn timeout_kills_group_and_reaps_direct_child() {
    let (dir, adb) = fake(
        "import os,time\nopen(__file__+'.pid','w').write(str(os.getpid()))\nos.fork()\ntime.sleep(30)",
    );
    let pid_file = dir.path().join("fake adb ; executable.pid");
    let actual = adb
        .inventory(Duration::from_millis(300), CancellationToken::new())
        .await
        .err();
    // Print only the stable safe code, never subprocess output/environment.
    eprintln!("timeout/reap safe ErrorCode: {actual:?}");
    assert_eq!(
        actual,
        Some(ErrorCode::ProcessTimeout),
        "safe supervisor error code"
    );
    let pid = std::fs::read_to_string(pid_file).unwrap();
    assert!(
        !std::path::Path::new(&format!("/proc/{pid}")).exists(),
        "direct child must be reaped"
    );
}
#[tokio::test]
async fn nonzero_exit_and_invalid_target_are_errors() {
    let (_dir, adb) = fake("raise SystemExit(1)");
    assert!(matches!(
        adb.inventory(Duration::from_secs(1), CancellationToken::new())
            .await,
        Err(ErrorCode::ProcessFailed)
    ));
    assert!(matches!(
        adb.screenshot(&Target { transport_id: 0 }, CancellationToken::new())
            .await,
        Err(ErrorCode::InvalidRequest)
    ));
}
#[test]
fn inventory_distinguishes_authorization_and_rejects_duplicate_transport_ids() {
    let inv=b"List of devices attached\nsynthetic device usb:1-2 product:test transport_id:9\nsecond unauthorized usb:1-3 transport_id:10\nthird offline transport_id:11\nfourth no permissions (missing udev rules) usb:1-4\n";
    let a = parse_inventory(inv).unwrap();
    assert_eq!(a.len(), 4);
    assert_eq!(a[1].state, Authorization::Unauthorized);
    assert_eq!(a[2].state, Authorization::Offline);
    assert_eq!(a[3].state, Authorization::NoPermissions);
    assert!(parse_inventory(b"unexpected output").is_err());
    assert!(
        parse_inventory(
            b"List of devices attached\na device transport_id:1\nb device transport_id:1\n"
        )
        .is_err()
    );
}
#[test]
fn png_signature_decode_and_resize_are_bounded() {
    let png = include_bytes!("fixtures/synthetic.png");
    let (out, size) = resize_png(png, 4).unwrap();
    assert_eq!(size.width, 2);
    assert_eq!(size.height, 4);
    assert!(out.starts_with(b"\x89PNG\r\n\x1a\n"));
    assert!(resize_png(b"not png", 1280).is_err());
    assert!(resize_png(&png[..24], 1280).is_err());
    assert!(resize_png(png, 0).is_err());
}
#[test]
fn source_backed_metadata_observes_180_and_rejects_unknowns() {
    use cordgeist::{api::Rotation, metadata::parse_aosp_viewport};
    let fixture = include_str!("fixtures/aosp-viewport.txt");
    let first = parse_aosp_viewport(fixture.as_bytes(), b"0\n").unwrap();
    assert_eq!(first.rotation, Rotation::R0);
    let changed = fixture.replace("orientation=0", "orientation=2");
    let after = parse_aosp_viewport(changed.as_bytes(), b"0\n").unwrap();
    assert_eq!(after.rotation, Rotation::R180);
    assert_ne!(first, after);
    for text in [
        fixture.replace("orientation=0", "orientation=9"),
        fixture.replace("isActive=[1]", "isActive=[0]"),
        fixture.replace("INTERNAL", "EXTERNAL"),
        fixture.replace("displayId=0", "displayId=1"),
        format!(
            "{fixture}\n    Viewport INTERNAL: displayId=0, uniqueId=local:other, port=<none>, orientation=1, logicalFrame=[0, 0, 4, 8], physicalFrame=[0, 0, 4, 8], deviceSize=[4, 8], isActive=[1]\n"
        ),
        "unknown OEM format".into(),
    ] {
        assert!(parse_aosp_viewport(text.as_bytes(), b"0").is_err());
    }
    assert!(parse_aosp_viewport(fixture.as_bytes(), b"10").is_err());
    let line = fixture
        .lines()
        .find(|line| line.contains("Viewport INTERNAL"))
        .unwrap()
        .trim();
    let repeated = format!("{fixture}\n    {line}\n    {line}\n");
    assert_eq!(
        parse_aosp_viewport(repeated.as_bytes(), b"0"),
        Ok(first.clone())
    );
    let omitted = fixture.replace(", isActive=[1]", "");
    assert_eq!(parse_aosp_viewport(omitted.as_bytes(), b"0"), Ok(first));
}
#[tokio::test]
async fn bracketed_capture_rejects_rotation_and_identity_change() {
    for mode in [
        "stable",
        "rotate",
        "identity",
        "capture-error",
        "invalid-png",
    ] {
        let script = format!(
            r#"
import sys,pathlib
args=sys.argv[1:]
assert args[:4]==['-H','127.0.0.1','-P','5039']
base=pathlib.Path(__file__)
if args[4:]==['devices','-l']:
 p=base.with_suffix('.inv');n=int(p.read_text()) if p.exists() else 0;p.write_text(str(n+1))
 serial='changed' if '{mode}'=='identity' and n else 'synthetic'
 print('List of devices attached\n'+serial+' device usb:1-2 transport_id:77\n')
elif args[4:]==['-t','77','shell','dumpsys','input']:
 p=base.with_suffix('.dump');n=int(p.read_text()) if p.exists() else 0;p.write_text(str(n+1))
 text=base.with_suffix('.viewport').read_text()
 if '{mode}'=='rotate' and n:text=text.replace('orientation=0','orientation=2')
 print(text)
elif args[4:]==['-t','77','shell','am','get-current-user']:print(0)
elif args[4:]==['-t','77','shell','wm','size']:print('Physical size: 4x8')
elif args[4:]==['-t','77','exec-out','screencap','-p']:
 if '{mode}'=='capture-error':raise SystemExit(1)
 sys.stdout.buffer.write(b'invalid PNG' if '{mode}'=='invalid-png' else base.with_suffix('.png').read_bytes())
else:raise RuntimeError(args)
"#
        );
        let (dir, adb) = fake(&script);
        let base = dir.path().join("fake adb ; executable");
        std::fs::write(
            base.with_extension("viewport"),
            include_bytes!("fixtures/aosp-viewport.txt"),
        )
        .unwrap();
        std::fs::write(
            base.with_extension("png"),
            include_bytes!("fixtures/synthetic.png"),
        )
        .unwrap();
        let expected = Attachment {
            serial: "synthetic".into(),
            state: Authorization::Authorized,
            transport_id: Some(77),
            usb: Some("1-2".into()),
        };
        let result = adb.observe(&expected, CancellationToken::new()).await;
        match mode {
            "stable" => {
                let o = result.unwrap();
                assert_eq!(o.image.width, 4);
                assert_eq!(o.image.height, 8);
            }
            "rotate" => assert!(matches!(result, Err(ErrorCode::GeometryChanged))),
            "capture-error" => assert!(matches!(result, Err(ErrorCode::ProcessFailed))),
            "invalid-png" => assert!(matches!(result, Err(ErrorCode::CaptureUnavailable))),
            _ => assert!(matches!(result, Err(ErrorCode::DeviceIdentityAmbiguous))),
        }
    }
}

#[tokio::test]
async fn dropping_public_inventory_waiter_keeps_explicit_kill_and_reap_owner() {
    let (dir, adb) = fake(
        "import os,time\nf=open(__file__+'.pid.tmp','w')\nf.write(str(os.getpid()))\nf.close()\nos.replace(__file__+'.pid.tmp',__file__+'.pid')\ntime.sleep(30)",
    );
    let pid_file = dir.path().join("fake adb ; executable.pid");
    let task = tokio::spawn(async move {
        adb.inventory(Duration::from_secs(5), CancellationToken::new())
            .await
    });
    tokio::time::timeout(Duration::from_secs(2), async {
        while !pid_file.exists() {
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
    })
    .await
    .unwrap();
    let marker = std::fs::read_to_string(pid_file).unwrap();
    let parsed = marker.parse::<u32>();
    eprintln!(
        "drop/reap marker-complete={} marker-bytes={}",
        parsed.as_ref().is_ok_and(|pid| *pid > 0),
        marker.len()
    );
    let pid = parsed.expect("fake ready marker must contain a complete positive PID");
    assert!(pid > 0, "fake ready marker must contain a positive PID");
    task.abort();
    let _ = task.await;
    // No subsequent Command spawn is allowed to trigger Tokio's deferred orphan
    // reaper: the independent supervisor must explicitly wait for its own child.
    tokio::time::timeout(Duration::from_secs(1), async {
        while std::path::Path::new(&format!("/proc/{pid}")).exists() {
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
    })
    .await
    .unwrap();
}

// SOFTWARE FAKE executable; production admission/supervisor API, no ADB server.
#[tokio::test]
async fn capacity_rejection_is_distinct_and_does_not_spawn_a_ninth_process() {
    use std::{future::Future, task::Poll};
    let (_dir, adb) = fake("raise AssertionError('cancelled queued supervisor must never spawn')");
    let cancel = CancellationToken::new();
    let mut pending = (0..8)
        .map(|_| Box::pin(adb.inventory(Duration::from_secs(1), cancel.clone())))
        .collect::<Vec<_>>();
    // One current-thread poll admits each call, without running its supervisor.
    std::future::poll_fn(|cx| {
        for call in &mut pending {
            assert!(call.as_mut().poll(cx).is_pending());
        }
        Poll::Ready(())
    })
    .await;
    assert!(matches!(
        adb.inventory(Duration::from_secs(1), cancel.clone()).await,
        Err(ErrorCode::QueueFull)
    ));
    cancel.cancel();
    for call in pending {
        assert!(matches!(call.await, Err(ErrorCode::ProcessCancelled)));
    }
}

#[tokio::test]
async fn inherited_writable_fake_source_cannot_make_inventory_executable_busy() {
    use std::os::fd::AsRawFd;
    let (dir, adb) = fake("print('software fixture ready')");
    let path = dir.path().join("fake adb ; executable");
    let writer = std::fs::OpenOptions::new()
        .write(true)
        .open(common::source(&path))
        .unwrap();
    let mut gate = [0; 2];
    assert_eq!(
        unsafe { libc::pipe2(gate.as_mut_ptr(), libc::O_CLOEXEC) },
        0
    );
    let child = unsafe { libc::fork() };
    assert!(child >= 0);
    if child == 0 {
        // Only async-signal-safe calls after fork. Keep the inherited writer
        // alive after the parent closes it; explicit wake, finite wait, no EOF.
        unsafe {
            libc::close(gate[1]);
            let mut ready = libc::pollfd {
                fd: gate[0],
                events: libc::POLLIN,
                revents: 0,
            };
            let polled = libc::poll(&mut ready, 1, 2000);
            let mut byte = 0u8;
            let count = if polled > 0 {
                libc::read(gate[0], (&mut byte as *mut u8).cast(), 1)
            } else {
                0
            };
            libc::close(writer.as_raw_fd());
            libc::_exit(if count == 1 && byte == 1 { 0 } else { 3 });
        }
    }
    unsafe {
        libc::close(gate[0]);
    }
    drop(writer);
    let result = adb
        .inventory(Duration::from_secs(1), CancellationToken::new())
        .await;
    let byte = 1u8;
    assert_eq!(
        unsafe { libc::write(gate[1], (&byte as *const u8).cast(), 1) },
        1
    );
    unsafe {
        libc::close(gate[1]);
    }
    let mut status = 0;
    assert_eq!(unsafe { libc::waitpid(child, &mut status, 0) }, child);
    assert!(libc::WIFEXITED(status));
    assert_eq!(libc::WEXITSTATUS(status), 0);
    assert_eq!(
        result.map(|out| out.stdout),
        Ok(b"software fixture ready\n".to_vec())
    );
}

#[tokio::test]
async fn synthetic_reverse_route_is_exact_and_never_replaces_a_conflict() {
    for mode in ["same", "conflict", "missing"] {
        let script = format!(
            r#"
import sys
assert sys.argv[1:7]==['-H','127.0.0.1','-P','5039','-t','77']
a=sys.argv[7:]
mode={mode:?}
if a==['reverse','--list']:
 if mode=='same': print('SYNTHETIC tcp:18764 tcp:18764')
 if mode=='conflict': print('SYNTHETIC tcp:18764 tcp:9999')
elif a==['reverse','--no-rebind','tcp:18764','tcp:18764']:
 assert mode=='missing','existing reverse route must never be replaced'
 open(__file__+'.prepared','w').close()
else: raise AssertionError('unexpected command')
"#
        );
        let (dir, adb) = fake(&script);
        let result = adb
            .prepare_test_area(&Target { transport_id: 77 }, CancellationToken::new())
            .await;
        assert_eq!(result.is_ok(), mode != "conflict");
        assert_eq!(
            dir.path().join("fake adb ; executable.prepared").exists(),
            mode == "missing"
        );
    }
}

#[tokio::test]
async fn ui_dump_idle_retry_owned_fallback_and_cleanup_are_exactly_routed() {
    let (dir, adb) = fake(
        r#"
import sys,pathlib
assert sys.argv[1:7]==['-H','127.0.0.1','-P','5039','-t','77']
a=sys.argv[7:];root=pathlib.Path(__file__).parent;state=root/'calls'
n=int(state.read_text()) if state.exists() else 0;state.write_text(str(n+1))
if a==['exec-out','timeout','3','uiautomator','dump','/dev/tty']:
 if n==0: print('ERROR: could not get idle state.')
elif a[:5]==['exec-out','timeout','3','uiautomator','dump']:
 assert a[5]=='--compressed' and a[6].startswith('/sdcard/Cordgeist-ui-') and a[6].endswith('.xml');(root/'owned').write_text(a[6])
elif a[:2]==['exec-out','cat']:
 assert a[2]==(root/'owned').read_text();print('<hierarchy><node text="SYNTHETIC" bounds="[0,0][1,1]"/></hierarchy>')
elif a[0]=='shell':
 assert a[1]=="rm '"+(root/'owned').read_text()+"'";(root/'cleaned').write_text('yes')
else: raise RuntimeError('unexpected command')
"#,
    );
    let xml = adb
        .ui_xml(&Target { transport_id: 77 }, CancellationToken::new())
        .await
        .unwrap();
    assert!(xml.starts_with(b"<hierarchy>"));
    assert_eq!(
        std::fs::read_to_string(dir.path().join("calls")).unwrap(),
        "4"
    );
    assert!(dir.path().join("cleaned").exists());
}
#[tokio::test]
async fn ui_stream_does_not_create_a_remote_file_and_apps_are_validated() {
    let (_dir, adb) = fake(
        r#"
import sys
assert sys.argv[1:7]==['-H','127.0.0.1','-P','5039','-t','77']
a=sys.argv[7:]
if a==['exec-out','timeout','3','uiautomator','dump','/dev/tty']: print('<hierarchy><node text="SYNTHETIC" bounds="[0,0][1,1]"/></hierarchy>')
elif a==['shell','pm list packages']: print('package:com.android.settings\npackage:bad;command\npackage:com.android.settings')
else: raise RuntimeError('unexpected command')
"#,
    );
    adb.ui_xml(&Target { transport_id: 77 }, CancellationToken::new())
        .await
        .unwrap();
    assert_eq!(
        adb.list_apps(&Target { transport_id: 77 }, CancellationToken::new())
            .await
            .unwrap(),
        vec!["com.android.settings"]
    );
    for bad in ["", "x;id", "x y", "$(id)", "-p"] {
        assert!(!valid_package(bad));
    }
}

#[cfg(feature = "native-aoa")]
#[tokio::test]
async fn authorized_android_controls_are_fixed_routed_and_reject_injection() {
    use cordgeist::api::{AndroidButton, AndroidCommand};
    let (dir, adb) = fake(
        r#"
import sys,pathlib
assert sys.argv[1:7]==['-H','127.0.0.1','-P','5039','-t','77']
a=sys.argv[7:];assert a[0]=='shell';assert a[1] in ['input keyevent 4','input keyevent 3','input keyevent 66','monkey -p com.android.settings -c android.intent.category.LAUNCHER 1'];pathlib.Path(__file__).with_suffix('.called').write_text('yes')
"#,
    );
    let target = Target { transport_id: 77 };
    for button in [
        AndroidButton::Back,
        AndroidButton::Home,
        AndroidButton::Enter,
    ] {
        assert!(
            adb.android_command(
                &target,
                &AndroidCommand::PressButton { button },
                std::time::Instant::now() + Duration::from_secs(1),
                CancellationToken::new()
            )
            .await
            .unwrap()
            .success
        );
    }
    assert!(
        adb.android_command(
            &target,
            &AndroidCommand::LaunchApp {
                package: "com.android.settings".into()
            },
            std::time::Instant::now() + Duration::from_secs(1),
            CancellationToken::new()
        )
        .await
        .unwrap()
        .success
    );
    assert!(dir.path().join("fake adb ; executable.called").exists());
    assert!(matches!(
        adb.android_command(
            &target,
            &AndroidCommand::LaunchApp {
                package: "x; touch /private".into()
            },
            std::time::Instant::now() + Duration::from_secs(1),
            CancellationToken::new()
        )
        .await,
        Err(ErrorCode::InvalidRequest)
    ));
    assert!(
        serde_json::from_value::<AndroidCommand>(
            serde_json::json!({"kind":"press-button","button":"VOLUME_UP"})
        )
        .is_err()
    );
}

#[tokio::test]
async fn ui_dump_hard_deadline_limits_hung_dump_and_one_retry() {
    // SOFTWARE process fixture, not hardware latency evidence.
    let (dir, adb) = fake(
        "import sys,time,pathlib\np=pathlib.Path(__file__).parent/'attempts'\np.write_text(p.read_text()+'x' if p.exists() else 'x')\ntime.sleep(30)",
    );
    let start = std::time::Instant::now();
    assert_eq!(
        adb.ui_xml(&Target { transport_id: 77 }, CancellationToken::new())
            .await
            .err(),
        Some(ErrorCode::ProcessTimeout)
    );
    assert!(start.elapsed() < Duration::from_secs(8));
    // Two dumps; a bounded cleanup command may also be attempted.
    let calls = std::fs::read_to_string(dir.path().join("attempts")).unwrap();
    assert!((2..=3).contains(&calls.len()));
}

#[tokio::test]
async fn names_resolve_on_exact_route_without_shell_interpolation_or_ambiguous_choice() {
    // SOFTWARE process fixture; actual native Settings resolution is measured separately.
    let (_dir, adb) = fake(
        r#"
import sys
assert sys.argv[1:7]==['-H','127.0.0.1','-P','5039','-t','77']
a=sys.argv[7:]
if a==['shell','cmd','package','resolve-activity','--brief','-a','android.settings.SETTINGS']:
 print('example.settings/.Main')
elif a==['shell','cmd','package','resolve-activity','--brief','-a','android.media.action.STILL_IMAGE_CAMERA']:
 print('example.camera/.Main')
elif a==['shell','pm list packages']:
 print('package:example.browser\npackage:example.chrome\npackage:one.reader\npackage:two.reader')
else:raise RuntimeError('unexpected command')
"#,
    );
    let target = Target { transport_id: 77 };
    assert_eq!(
        adb.resolve_app(&target, "Settings", CancellationToken::new())
            .await
            .unwrap(),
        "example.settings"
    );
    for (name, package) in [("Chrome", "example.chrome"), ("Camera", "example.camera")] {
        assert_eq!(
            adb.resolve_app(&target, name, CancellationToken::new())
                .await
                .unwrap(),
            package
        );
    }
    assert_eq!(
        adb.resolve_app(&target, "Missing app", CancellationToken::new())
            .await
            .err(),
        Some(ErrorCode::InvalidRequest)
    );
    assert_eq!(
        adb.resolve_app(&target, "Browser", CancellationToken::new())
            .await
            .unwrap(),
        "example.browser"
    );
    assert_eq!(
        adb.resolve_app(&target, "reader", CancellationToken::new())
            .await
            .err(),
        Some(ErrorCode::InvalidRequest)
    );
    assert_eq!(
        adb.resolve_app(&target, "Settings; echo secret", CancellationToken::new())
            .await
            .err(),
        Some(ErrorCode::InvalidRequest)
    );
}
