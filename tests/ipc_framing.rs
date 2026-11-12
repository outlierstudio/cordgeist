#![cfg(target_os = "linux")]
//! SOFTWARE transport tests only; no device/backend calls.
use cordgeist::ipc::framing::*;
use std::{
    io::{ErrorKind, Write},
    os::unix::net::UnixStream,
    time::{Duration, Instant},
};
fn deadline() -> Instant {
    Instant::now() + MESSAGE_TIMEOUT
}
#[test]
fn frames_roundtrip_and_preserve_boundaries() {
    let (mut a, mut b) = UnixStream::pair().unwrap();
    send(&mut a, &vec![1, 2, 3], deadline()).unwrap();
    send(&mut a, &false, deadline()).unwrap();
    assert_eq!(receive::<Vec<u8>>(&mut b, deadline()).unwrap(), [1, 2, 3]);
    assert!(!receive::<bool>(&mut b, deadline()).unwrap());
}
#[test]
fn malformed_truncated_and_oversized_frames_fail_closed() {
    for frame in [
        vec![0, 0, 0, 0],
        ((MAX_MESSAGE + 1) as u32).to_be_bytes().to_vec(),
        vec![0, 0, 0, 1, b'{'],
        vec![0, 0, 0, 2, b'1'],
        vec![0, 0],
    ] {
        let (mut a, mut b) = UnixStream::pair().unwrap();
        a.write_all(&frame).unwrap();
        drop(a);
        assert!(receive::<serde_json::Value>(&mut b, deadline()).is_err());
    }
}
#[test]
fn expired_and_incomplete_messages_are_bounded_and_disconnect_is_safe() {
    let (mut a, mut b) = UnixStream::pair().unwrap();
    assert_eq!(
        receive::<bool>(&mut b, Instant::now()).unwrap_err().kind(),
        ErrorKind::TimedOut
    );
    a.write_all(&[0, 0]).unwrap();
    let start = Instant::now();
    assert!(receive::<bool>(&mut b, start + Duration::from_millis(30)).is_err());
    assert!(start.elapsed() < Duration::from_secs(1));
    drop(b);
    assert!(send(&mut a, &true, deadline()).is_err());
}
