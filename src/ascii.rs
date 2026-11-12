//! Explicit US ANSI physical-key policy; no actual Android layout verification.
//! Payload intentionally has no Debug/Serialize and is never persisted or logged.
use crate::{api::ErrorCode, session_input::Transition};
/// Private text must not acquire diagnostic formatting or wire serialization.
/// ```compile_fail
/// use cordgeist::ascii::Text;
/// let private = Text::new("us-ansi", "synthetic");
/// let _ = format!("{private:?}");
/// ```
/// ```compile_fail
/// use cordgeist::ascii::Text;
/// let private = Text::new("us-ansi", "synthetic");
/// let _ = serde_json::to_string(&private);
/// ```
#[derive(Clone)]
pub struct Text {
    layout: String,
    payload: String,
}
impl Text {
    pub fn new(layout: impl Into<String>, payload: impl Into<String>) -> Self {
        Self {
            layout: layout.into(),
            payload: payload.into(),
        }
    }
    pub fn plan(&self) -> Result<Vec<Vec<Transition>>, ErrorCode> {
        if self.layout != "us-ansi" {
            return Err(ErrorCode::InvalidRequest);
        }
        if self.payload.is_empty() || self.payload.len() > 4096 {
            return Err(ErrorCode::InvalidRequest);
        }
        self.payload
            .bytes()
            .map(|byte| {
                let (usage, shift) = match byte {
                    b'a'..=b'z' => (4 + u16::from(byte - b'a'), false),
                    b'A'..=b'Z' => (4 + u16::from(byte - b'A'), true),
                    b'1'..=b'9' => (0x1e + u16::from(byte - b'1'), false),
                    b'0' => (0x27, false),
                    b' ' => (0x2c, false),
                    b'\t' => (0x2b, false),
                    b'\n' => (0x28, false),
                    _ => {
                        const BASE: &[u8] = b"-=[]\\;'`,./";
                        const SHIFT: &[u8] = b"_+{}|:\"~<>?";
                        const USAGES: [u16; 11] = [
                            0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
                        ];
                        if let Some(i) = BASE.iter().position(|b| *b == byte) {
                            (USAGES[i], false)
                        } else if let Some(i) = SHIFT.iter().position(|b| *b == byte) {
                            (USAGES[i], true)
                        } else if let Some(i) = b"!@#$%^&*()".iter().position(|b| *b == byte) {
                            (if i == 9 { 0x27 } else { 0x1e + i as u16 }, true)
                        } else {
                            return Err(ErrorCode::UnsupportedCharacter);
                        }
                    }
                };
                let mut chord = Vec::with_capacity(4);
                if shift {
                    chord.push(Transition::Key {
                        usage: 0xe1,
                        down: true,
                    });
                }
                chord.push(Transition::Key { usage, down: true });
                chord.push(Transition::Key { usage, down: false });
                if shift {
                    chord.push(Transition::Key {
                        usage: 0xe1,
                        down: false,
                    });
                }
                Ok(chord)
            })
            .collect()
    }
}
