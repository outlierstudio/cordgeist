//! Owned static synthetic test content only. No phone authority, files or model data.
use std::{
    io::{self, Read, Write},
    net::TcpListener,
    time::{Duration, Instant},
};

pub fn serve(port: u16, seconds: u64) -> io::Result<()> {
    if port == 0 || !(1..=900).contains(&seconds) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let listener = TcpListener::bind(("127.0.0.1", port))?;
    listener.set_nonblocking(true)?;
    println!("Synthetic test page: http://127.0.0.1:{port}/ (expires in {seconds} seconds)");
    let deadline = Instant::now() + Duration::from_secs(seconds);
    while Instant::now() < deadline {
        match listener.accept() {
            Ok((mut stream, _)) => {
                stream.set_read_timeout(Some(Duration::from_secs(1)))?;
                stream.set_write_timeout(Some(Duration::from_secs(1)))?;
                let mut request = [0u8; 1024];
                let Ok(count) = stream.read(&mut request) else {
                    continue;
                };
                let first = request[..count]
                    .split(|b| *b == b'\n')
                    .next()
                    .unwrap_or_default();
                if first != b"GET / HTTP/1.1\r" && first != b"GET / HTTP/1.0\r" {
                    let _ = stream.write_all(
                        b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                    );
                    continue;
                }
                let page = include_bytes!("../assets/v1-test-area.html");
                let headers = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                    page.len()
                );
                if stream.write_all(headers.as_bytes()).is_ok() {
                    let _ = stream.write_all(page);
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20))
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
