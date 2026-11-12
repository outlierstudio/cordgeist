//! Real socket and reusable client. No device backend.
#![cfg(target_os = "linux")]
use cordgeist::{
    api::*,
    ipc::{
        client::{self, ClientError},
        framing,
        server::{Hello, Server, Welcome},
    },
};
use std::{
    io::ErrorKind,
    os::unix::{fs::PermissionsExt, net::UnixListener},
    time::{Duration, Instant},
};
fn private_dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
#[test]
fn expired_deadline_does_not_connect() {
    let err = match client::exchange(
        std::path::Path::new("/tmp/cordgeist-missing.sock"),
        Call::Status,
        Instant::now() - Duration::from_millis(1),
    ) {
        Err(err) => err,
        Ok(_) => panic!("deadline must fail closed"),
    };
    assert!(matches!(err, ClientError::Expired));
}
#[test]
fn incompatible_welcome_is_not_retried() {
    let dir = private_dir();
    let path = dir.path().join("sock");
    let listener = UnixListener::bind(&path).unwrap();
    listener.set_nonblocking(true).unwrap();
    let server = std::thread::spawn(move || {
        let mut accepts = 0;
        let end = Instant::now() + Duration::from_secs(2);
        while Instant::now() < end {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    accepts += 1;
                    let deadline = Instant::now() + Duration::from_secs(1);
                    let _: Hello = framing::receive(&mut stream, deadline).unwrap();
                    framing::send(
                        &mut stream,
                        &Welcome {
                            version: ProtocolVersion,
                            boot_id: BootId("boot".into()),
                            compatible: false,
                            device_control_available: false,
                        },
                        deadline,
                    )
                    .unwrap();
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(5))
                }
                Err(_) => break,
            }
        }
        accepts
    });
    let err = match client::exchange(
        &path,
        Call::Status,
        Instant::now() + Duration::from_millis(500),
    ) {
        Err(err) => err,
        Ok(_) => panic!("incompatible daemon must be rejected"),
    };
    assert!(matches!(err, ClientError::Incompatible));
    assert_eq!(server.join().unwrap(), 1);
}
#[test]
fn lost_reply_is_uncertain_and_not_resent() {
    let dir = private_dir();
    let path = dir.path().join("sock");
    let listener = UnixListener::bind(&path).unwrap();
    listener.set_nonblocking(true).unwrap();
    let server = std::thread::spawn(move || {
        let mut accepts = 0;
        let end = Instant::now() + Duration::from_secs(2);
        while Instant::now() < end {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    accepts += 1;
                    let deadline = Instant::now() + Duration::from_secs(1);
                    let _: Hello = framing::receive(&mut stream, deadline).unwrap();
                    framing::send(
                        &mut stream,
                        &Welcome {
                            version: ProtocolVersion,
                            boot_id: BootId("boot".into()),
                            compatible: true,
                            device_control_available: false,
                        },
                        deadline,
                    )
                    .unwrap();
                    let _: Request = framing::receive(&mut stream, deadline).unwrap();
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(5))
                }
                Err(_) => break,
            }
        }
        accepts
    });
    let err = match client::exchange(
        &path,
        Call::Tap {
            auth: sample_auth(),
            seq: ActionSeq(1),
            frame_id: FrameId("frame".into()),
            point: Point { x: 1.0, y: 1.0 },
        },
        Instant::now() + Duration::from_millis(200),
    ) {
        Err(err) => err,
        Ok(_) => panic!("lost reply must stay uncertain"),
    };
    assert!(matches!(err, ClientError::Uncertain(_)));
    assert_eq!(server.join().unwrap(), 1);
}
#[test]
fn status_round_trip_correlates_boot_and_request() {
    let dir = private_dir();
    let server = Server::bind(dir.path()).unwrap();
    let stop = server.stop_handle();
    let boot = server.boot_id().clone();
    let path = dir.path().join("cordgeist/daemon.sock");
    let running = std::thread::spawn(move || server.run().unwrap());
    let response = client::exchange(
        &path,
        Call::Status,
        Instant::now() + Duration::from_millis(800),
    )
    .unwrap();
    assert_eq!(response.boot_id, boot);
    assert!(!response.request_id.is_empty());
    assert!(
        matches!(response.outcome, ResponseOutcome::Error { error } if error.code == ErrorCode::BackendUnavailable)
    );
    stop.request();
    running.join().unwrap();
}
fn sample_auth() -> LeaseAuth {
    LeaseAuth {
        boot_id: BootId("boot".into()),
        device_id: DeviceId("phone".into()),
        lease_id: LeaseId("lease".into()),
        credential: Credential("secret".into()),
        attachment: AttachmentGeneration(1),
        fence: Fence(1),
    }
}
