//! PNG and sidecar publication. No device.
#![cfg(target_os = "linux")]
use cordgeist::{api::*, export};
use std::os::unix::fs::PermissionsExt;
fn dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
fn png() -> (Vec<u8>, String) {
    let bytes = include_bytes!("fixtures/synthetic.png").to_vec();
    let encoded = base64(&bytes);
    (bytes, encoded)
}
fn base64(bytes: &[u8]) -> String {
    const TABLE: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in bytes.chunks(3) {
        let n = (u32::from(c[0]) << 16)
            | (u32::from(*c.get(1).unwrap_or(&0)) << 8)
            | u32::from(*c.get(2).unwrap_or(&0));
        out.push(TABLE[((n >> 18) & 63) as usize] as char);
        out.push(TABLE[((n >> 12) & 63) as usize] as char);
        out.push(if c.len() > 1 {
            TABLE[((n >> 6) & 63) as usize] as char
        } else {
            '='
        });
        out.push(if c.len() > 2 {
            TABLE[(n & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}
fn sample(width: u32, height: u32) -> Frame {
    Frame {
        id: FrameId("frame-1".into()),
        provenance: Provenance {
            boot_id: BootId("boot".into()),
            device_id: DeviceId("phone".into()),
            lease_id: LeaseId("lease".into()),
            attachment: AttachmentGeneration(1),
            fence: Fence(1),
            geometry: GeometryVersion(1),
            mapping: MappingVersion(1),
        },
        captured_ms: 1,
        image: Size { width, height },
        display: Size { width, height },
        rotation: Rotation::R0,
        display_id: 0,
        coordinate_space: CoordinateSpace::ReturnedImagePixels,
    }
}
#[test]
fn export_matches_frame_and_refuses_overwrite_or_mismatch() {
    let dir = dir();
    let dest = dir.path().join("shot.png");
    let (bytes, encoded) = png();
    let (width, height) = png_size(&bytes);
    let image = PngImage {
        mime_type: PngMime::Png,
        base64: encoded,
    };
    let frame = sample(width, height);
    export::publish(&image, &frame, &dest, false).unwrap();
    let saved = png_size(&std::fs::read(&dest).unwrap());
    assert_eq!(saved, (width, height));
    let meta: serde_json::Value =
        serde_json::from_slice(&std::fs::read(export::metadata_path(&dest)).unwrap()).unwrap();
    assert_eq!(meta["frame"]["id"], "frame-1");
    assert_eq!(meta["frame"]["image"]["width"], width);
    assert_eq!(meta["frame"]["image"]["height"], height);
    assert!(export::publish(&image, &frame, &dest, false).is_err());
    let partial = dir.path().join("partial.png");
    export::publish(&image, &frame, &partial, true).unwrap();
    assert!(partial.is_file());
    assert!(!export::metadata_path(&partial).exists());
    let bad = dir.path().join("bad.png");
    assert!(export::publish(&image, &sample(2, 2), &bad, false).is_err());
    assert!(!bad.exists());
}
fn png_size(bytes: &[u8]) -> (u32, u32) {
    assert_eq!(&bytes[12..16], b"IHDR");
    (
        u32::from_be_bytes(bytes[16..20].try_into().unwrap()),
        u32::from_be_bytes(bytes[20..24].try_into().unwrap()),
    )
}
