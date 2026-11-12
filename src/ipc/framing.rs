//! Four-byte network-order JSON framing with an absolute per-message deadline.
use serde::{Serialize, de::DeserializeOwned};
use std::{
    io::{self, Read, Write},
    os::unix::net::UnixStream,
    time::{Duration, Instant},
};

pub const MAX_MESSAGE: usize = 8 * 1024 * 1024;
pub const MESSAGE_TIMEOUT: Duration = Duration::from_secs(2);
pub const CALIBRATION_TIMEOUT: Duration = Duration::from_secs(60);
pub const SCREENSHOT_TIMEOUT: Duration = Duration::from_secs(10);
fn remaining(deadline: Instant) -> io::Result<Duration> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .ok_or_else(|| io::Error::from(io::ErrorKind::TimedOut))
}
fn read_exact(stream: &mut UnixStream, bytes: &mut [u8], deadline: Instant) -> io::Result<()> {
    let mut offset = 0;
    while offset < bytes.len() {
        stream.set_read_timeout(Some(remaining(deadline)?))?;
        match stream.read(&mut bytes[offset..]) {
            Ok(0) => return Err(io::ErrorKind::UnexpectedEof.into()),
            Ok(n) => offset += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    remaining(deadline)?;
    Ok(())
}
pub fn receive<T: DeserializeOwned>(stream: &mut UnixStream, deadline: Instant) -> io::Result<T> {
    let mut header = [0; 4];
    read_exact(stream, &mut header, deadline)?;
    let size = u32::from_be_bytes(header) as usize;
    if size == 0 || size > MAX_MESSAGE {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let mut body = vec![0; size];
    read_exact(stream, &mut body, deadline)?;
    // Never include serde's payload-bearing error details in diagnostics.
    let value =
        serde_json::from_slice(&body).map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    remaining(deadline)?;
    Ok(value)
}
pub fn send<T: Serialize>(stream: &mut UnixStream, value: &T, deadline: Instant) -> io::Result<()> {
    // Bound serialization as well as wire allocation.
    struct Limited(Vec<u8>);
    impl Write for Limited {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            if bytes.len() > MAX_MESSAGE.saturating_sub(self.0.len()) {
                return Err(io::ErrorKind::InvalidData.into());
            }
            self.0.extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let mut body = Limited(Vec::new());
    serde_json::to_writer(&mut body, value)
        .map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    let header = (body.0.len() as u32).to_be_bytes();
    for bytes in [&header[..], &body.0] {
        let mut offset = 0;
        while offset < bytes.len() {
            stream.set_write_timeout(Some(remaining(deadline)?))?;
            match stream.write(&bytes[offset..]) {
                Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
                Ok(n) => offset += n,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(e) => return Err(e),
            }
        }
    }
    remaining(deadline)?;
    Ok(())
}
