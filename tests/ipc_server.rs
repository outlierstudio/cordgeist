#![cfg(target_os = "linux")]
//! SOFTWARE test service: production IPC, explicitly unavailable device control.
use cordgeist::{
    api::*,
    ipc::{framing::*, server::*},
};
use std::{
    fs,
    os::unix::{fs::PermissionsExt, net::UnixStream},
    thread,
    time::Instant,
};
#[test]
fn public_service_handshake_typed_unavailable_and_stop() {
    let d = tempfile::tempdir().unwrap();
    fs::set_permissions(d.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind(d.path()).unwrap();
    let boot = server.boot_id().clone();
    let stop = server.stop_handle();
    let path = d.path().join("cordgeist/daemon.sock");
    let run = thread::spawn(move || server.run().unwrap());
    for version in [9, 1] {
        let mut stream = UnixStream::connect(&path).unwrap();
        let deadline = Instant::now() + MESSAGE_TIMEOUT;
        send(&mut stream, &Hello { version }, deadline).unwrap();
        let welcome: Welcome = receive(&mut stream, deadline).unwrap();
        assert_eq!(welcome.compatible, version == 1);
        assert!(!welcome.device_control_available);
        if version == 1 {
            let req = Request {
                coordinate_space: cordgeist::api::CoordinateSpace::ReturnedImagePixels,
                observe_after: false,
                version: 1,
                request_id: "synthetic-request".into(),
                deadline_ms: 1000,
                call: Call::Status,
            };
            send(&mut stream, &req, deadline).unwrap();
            let response: Response = receive(&mut stream, deadline).unwrap();
            response.validate_for(&req, &boot).unwrap();
            assert!(matches!(
                response.outcome,
                ResponseOutcome::Error {
                    error: ApiError {
                        code: ErrorCode::BackendUnavailable,
                        ..
                    }
                }
            ));
        }
    }
    let incomplete = UnixStream::connect(&path).unwrap();
    stop.request();
    run.join().unwrap();
    drop(incomplete);
    assert!(!path.exists());
}

#[test]
fn malformed_clients_do_not_prevent_new_handshakes_or_bounded_stop() {
    use std::io::{Read, Write};
    use std::time::Duration;
    let d = tempfile::tempdir().unwrap();
    fs::set_permissions(d.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind(d.path()).unwrap();
    let stop = server.stop_handle();
    let path = d.path().join("cordgeist/daemon.sock");
    let run = thread::spawn(move || server.run().unwrap());
    for frame in [
        vec![0, 0, 0, 0],
        vec![0, 0, 0, 1, b'{'],
        ((MAX_MESSAGE + 1) as u32).to_be_bytes().to_vec(),
        vec![0, 0],
    ] {
        let mut client = UnixStream::connect(&path).unwrap();
        client.set_read_timeout(Some(MESSAGE_TIMEOUT)).unwrap();
        client.write_all(&frame).unwrap();
        client.shutdown(std::net::Shutdown::Write).unwrap();
        let mut byte = [0];
        assert!(matches!(client.read(&mut byte), Ok(0)));
    }
    // Each welcome proves its worker was admitted before the next idle client.
    let mut held = Vec::new();
    for _ in 0..MAX_CONNECTIONS {
        let mut client = UnixStream::connect(&path).unwrap();
        let deadline = Instant::now() + MESSAGE_TIMEOUT;
        send(&mut client, &Hello { version: 1 }, deadline).unwrap();
        let _: Welcome = receive(&mut client, deadline).unwrap();
        held.push(client);
    }
    let mut excess = UnixStream::connect(&path).unwrap();
    excess
        .set_read_timeout(Some(Duration::from_secs(1)))
        .unwrap();
    assert_eq!(excess.read(&mut [0]).unwrap(), 0);
    let start = Instant::now();
    stop.request();
    run.join().unwrap();
    assert!(start.elapsed() < MESSAGE_TIMEOUT + Duration::from_secs(1));
    assert!(!path.exists());
    drop(held);
}
