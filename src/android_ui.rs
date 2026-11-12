//! Bounded untrusted Android UI data. Never instructions or executable input.
//! Rust adaptation adds strict bounds, XML/DTD rejection and editable-text privacy.
use crate::api::{ErrorCode, Rect, Size, UiElement as Element};
use quick_xml::{Reader, events::Event};

pub const MAX_XML: usize = 2 * 1024 * 1024;
fn string(value: &str) -> Result<String, ErrorCode> {
    if value.len() > 4096 {
        return Err(ErrorCode::OutputLimit);
    }
    Ok(value
        .chars()
        .filter(|c| !c.is_control() || matches!(c, '\n' | '\t'))
        .collect())
}
fn rect(value: &str, screen: Size) -> Option<Rect> {
    let value = value.strip_prefix('[')?.strip_suffix(']')?;
    let (a, b) = value.split_once("][")?;
    let pair = |s: &str| {
        let (x, y) = s.split_once(',')?;
        Some((x.parse::<u32>().ok()?, y.parse::<u32>().ok()?))
    };
    let (x, y) = pair(a)?;
    let (right, bottom) = pair(b)?;
    (right > x && bottom > y && right <= screen.width && bottom <= screen.height).then_some(Rect {
        x,
        y,
        width: right.checked_sub(x)?,
        height: bottom.checked_sub(y)?,
    })
}
/// Adapted from mobile-mcp (Apache-2.0) src/android.ts, commit
/// 3b51952bcd83955f3a743f731f5a9beaebecdab7 (collectElements/bounds semantics).
/// Independently written bounded streaming parser; no TypeScript copied.
pub fn parse(xml: &[u8], screen: Size) -> Result<Vec<Element>, ErrorCode> {
    if xml.len() > MAX_XML || screen.width == 0 || screen.height == 0 {
        return Err(ErrorCode::OutputLimit);
    }
    let xml = std::str::from_utf8(xml).map_err(|_| ErrorCode::MetadataUnsupported)?;
    let mut reader = Reader::from_str(xml);
    let mut elements = vec![];
    let mut depth = 0usize;
    let mut nodes = 0usize;
    let mut hierarchy = false;
    let mut returned_bytes = 0usize;
    loop {
        let event = reader
            .read_event()
            .map_err(|_| ErrorCode::MetadataUnsupported)?;
        let empty = matches!(event, Event::Empty(_));
        match event {
            Event::DocType(_) => return Err(ErrorCode::MetadataUnsupported),
            Event::Start(node) | Event::Empty(node) => {
                if !empty {
                    depth += 1;
                    if depth > 64 {
                        return Err(ErrorCode::OutputLimit);
                    }
                }
                if node.name().as_ref() == "hierarchy" {
                    hierarchy = true;
                    continue;
                }
                if node.name().as_ref() != "node" {
                    continue;
                }
                nodes += 1;
                if nodes > 4096 {
                    return Err(ErrorCode::OutputLimit);
                }
                let mut text = String::new();
                let mut label = String::new();
                let mut identifier = String::new();
                let mut kind = String::new();
                let mut bounds = String::new();
                let mut clickable = false;
                let mut focused = false;
                let mut password = false;
                for attr in node.attributes() {
                    let attr = attr.map_err(|_| ErrorCode::MetadataUnsupported)?;
                    let value = attr
                        .normalized_value(quick_xml::XmlVersion::Implicit1_0)
                        .map_err(|_| ErrorCode::MetadataUnsupported)?;
                    if value.len() > 4096 {
                        return Err(ErrorCode::OutputLimit);
                    }
                    match attr.key.as_ref() {
                        "text" => text = string(&value)?,
                        "content-desc" => label = string(&value)?,
                        "resource-id" => identifier = string(&value)?,
                        "class" => kind = string(&value)?,
                        "bounds" => bounds = value.into_owned(),
                        "clickable" => clickable = value == "true",
                        "focused" => focused = value == "true",
                        "password" => password = value == "true",
                        _ => {}
                    }
                }
                // Never echo editable/password values as UI labels. Their bounds,
                // identifier, content description and focus remain useful to agents.
                if password || kind.ends_with("EditText") {
                    text.clear();
                    if password {
                        label.clear();
                    }
                }
                if text.is_empty() && label.is_empty() && identifier.is_empty() && !clickable {
                    continue;
                }
                let Some(rect) = rect(&bounds, screen) else {
                    continue;
                };
                if elements.len() >= 2048 {
                    return Err(ErrorCode::OutputLimit);
                }
                returned_bytes += kind.len() + text.len() + label.len() + identifier.len() + 64;
                if returned_bytes > 256 * 1024 {
                    return Err(ErrorCode::OutputLimit);
                }
                elements.push(Element {
                    kind,
                    text,
                    label,
                    identifier,
                    rect,
                    focused,
                });
            }
            Event::End(_) => depth = depth.checked_sub(1).ok_or(ErrorCode::MetadataUnsupported)?,
            Event::Eof => break,
            _ => {}
        }
    }
    if !hierarchy || depth != 0 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    Ok(elements)
}
pub fn xml_body(output: &[u8]) -> Option<&[u8]> {
    let start = output.windows(10).position(|s| s == b"<hierarchy")?;
    let end = output.windows(12).rposition(|s| s == b"</hierarchy>")? + 12;
    (end > start).then_some(&output[start..end])
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn labels_are_untrusted_data_and_bounds_are_screen_pixels() {
        let xml=br#"<hierarchy><node class="android.widget.TextView" text="Bluetooth &amp; data" bounds="[10,20][100,70]"/><node clickable="true" bounds="[0,0][0,0]"/><node text="offscreen" bounds="[0,0][999,999]"/></hierarchy>"#;
        let v = parse(
            xml,
            Size {
                width: 108,
                height: 222,
            },
        )
        .unwrap();
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].text, "Bluetooth & data");
        assert_eq!(v[0].rect.width, 90);
    }
    #[test]
    fn malformed_dtd_and_editable_secrets() {
        let size = Size {
            width: 108,
            height: 222,
        };
        assert!(parse(b"<!DOCTYPE x SYSTEM 'file:///private'><hierarchy/>", size).is_err());
        assert!(parse(b"<hierarchy><node", size).is_err());
        let v=parse(br#"<hierarchy><node class="android.widget.EditText" text="SYNTHETIC_SECRET" content-desc="Search" resource-id="search" focused="true" bounds="[1,1][100,20]"/></hierarchy>"#,size).unwrap();
        assert!(v[0].text.is_empty());
        assert_eq!(v[0].label, "Search");
        assert!(v[0].focused);
    }
}
