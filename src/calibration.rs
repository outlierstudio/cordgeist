//! Read-only Android mapping evidence. Never grants verified control.
use crate::api::{ErrorCode, Size};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AxisEvidence {
    pub maximum_x: u16,
    pub maximum_y: u16,
    pub display: Size,
}

/// Narrow Android 10 input-mapper adapter. Only one external direct-touch device
/// with the exact host descriptor ranges and portrait primary surface is accepted.
/// Caller must correlate this with registration/discovery before trusting it.
pub fn android_axes(
    dump: &[u8],
    expected: Size,
    maxima: (u16, u16),
) -> Result<AxisEvidence, ErrorCode> {
    let reject = ErrorCode::MetadataUnsupported;
    if dump.len() > 1024 * 1024 || expected.width < 2 || expected.height <= expected.width {
        return Err(reject);
    }
    let text = std::str::from_utf8(dump).map_err(|_| reject)?;
    let mut candidate = None;
    let mut block = String::new();
    let mut blocks = Vec::new();
    for line in text.lines() {
        if line.starts_with("  Device ") {
            blocks.push(std::mem::take(&mut block));
        }
        block.push_str(line);
        block.push('\n');
    }
    blocks.push(block);
    for block in blocks {
        if !block.lines().any(|l| l.trim() == "IsExternal: true")
            || !block
                .lines()
                .any(|l| l.trim() == "Touch Input Mapper (mode - direct):")
        {
            continue;
        }
        let one = |prefix: &str| -> Result<String, ErrorCode> {
            let mut values = block.lines().filter_map(|l| l.trim().strip_prefix(prefix));
            let value = values.next().ok_or(reject)?;
            if values.next().is_some() {
                return Err(reject);
            }
            Ok(value.to_owned())
        };
        let width = one("SurfaceWidth: ")?
            .strip_suffix("px")
            .ok_or(reject)?
            .parse::<u32>()
            .map_err(|_| reject)?;
        let height = one("SurfaceHeight: ")?
            .strip_suffix("px")
            .ok_or(reject)?
            .parse::<u32>()
            .map_err(|_| reject)?;
        if one("SurfaceOrientation: ")? != "0" || (Size { width, height }) != expected {
            return Err(ErrorCode::GeometryChanged);
        }
        let raw = block.split_once("Raw Touch Axes:").ok_or(reject)?.1;
        let axis = |name: &str| -> Result<u16, ErrorCode> {
            let mut lines = raw.lines().filter_map(|l| l.trim().strip_prefix(name));
            let line = lines.next().ok_or(reject)?;
            if lines.next().is_some() {
                return Err(reject);
            }
            let max = line
                .strip_prefix("min=0, max=")
                .ok_or(reject)?
                .split_once(',')
                .ok_or(reject)?
                .0;
            max.parse().map_err(|_| reject)
        };
        let x = axis("X: ")?;
        let y = axis("Y: ")?;
        if (x, y) != maxima || x == 0 || y == 0 || candidate.is_some() {
            return Err(ErrorCode::MappingUnverified);
        }
        candidate = Some(AxisEvidence {
            maximum_x: x,
            maximum_y: y,
            display: expected,
        });
    }
    candidate.ok_or(ErrorCode::MappingUnverified)
}

/// Bounded independent-target verification, owned by daemon setup. Coordinates
/// come from a deliberate synthetic target observation, never IPC assertions.
/// This proof alone cannot install a native profile.
pub struct TargetCheck {
    deadline: std::time::Instant,
    size: Size,
    next: usize,
    failed: bool,
}
impl TargetCheck {
    pub fn new(size: Size, deadline: std::time::Instant) -> Result<Self, ErrorCode> {
        let now = std::time::Instant::now();
        if size.width < 100
            || size.height <= size.width
            || deadline <= now
            || deadline.duration_since(now) > std::time::Duration::from_secs(120)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        Ok(Self {
            deadline,
            size,
            next: 0,
            failed: false,
        })
    }
    pub fn target(&mut self) -> Result<crate::api::Point, ErrorCode> {
        if self.failed || std::time::Instant::now() >= self.deadline || self.next >= 3 {
            self.failed = true;
            return Err(ErrorCode::MappingUnverified);
        }
        let (x, y) = [(0.5, 0.5), (0.1, 0.15), (0.9, 0.85)][self.next];
        Ok(crate::api::Point {
            x: x * f64::from(self.size.width - 1),
            y: y * f64::from(self.size.height - 1),
        })
    }
    pub fn observe(&mut self, observed: crate::api::Point) -> Result<bool, ErrorCode> {
        let expected = self.target()?;
        // A narrow pixel tolerance, independent of target hit-box dimensions.
        if !observed.x.is_finite()
            || !observed.y.is_finite()
            || (observed.x - expected.x).abs() > 3.0
            || (observed.y - expected.y).abs() > 3.0
        {
            self.failed = true;
            return Err(ErrorCode::MappingUnverified);
        }
        self.next += 1;
        Ok(self.next == 3)
    }
    pub fn cancel(&mut self) {
        self.failed = true;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const BLOCK: &str = "  Device 29:\n    IsExternal: true\n    Touch Input Mapper (mode - direct):\n      Raw Touch Axes:\n        X: min=0, max=32767, flat=0\n        Y: min=0, max=32767, flat=0\n      SurfaceWidth: 1080px\n      SurfaceHeight: 2220px\n      SurfaceOrientation: 0\n";
    #[test]
    fn observed_axes_reject_ambiguity_changed_geometry_and_descriptor() {
        let size = Size {
            width: 1080,
            height: 2220,
        };
        assert_eq!(
            android_axes(BLOCK.as_bytes(), size, (32767, 32767))
                .unwrap()
                .display,
            size
        );
        assert!(android_axes(format!("{BLOCK}{BLOCK}").as_bytes(), size, (32767, 32767)).is_err());
        assert!(
            android_axes(
                BLOCK.replace("Orientation: 0", "Orientation: 1").as_bytes(),
                size,
                (32767, 32767)
            )
            .is_err()
        );
        assert!(android_axes(BLOCK.as_bytes(), size, (4095, 4095)).is_err());
        assert!(
            android_axes(
                BLOCK
                    .replace("IsExternal: true", "IsExternal: false")
                    .as_bytes(),
                size,
                (32767, 32767)
            )
            .is_err()
        );
    }
}

#[cfg(test)]
mod target_tests {
    use super::*;
    use std::time::{Duration, Instant};
    #[test]
    fn independent_targets_require_all_observations_and_failure_is_terminal() {
        let size = Size {
            width: 1080,
            height: 2220,
        };
        let mut check = TargetCheck::new(size, Instant::now() + Duration::from_secs(30)).unwrap();
        for i in 0..3 {
            let point = check.target().unwrap();
            assert_eq!(check.observe(point).unwrap(), i == 2);
        }
        assert!(check.target().is_err());
        let mut check = TargetCheck::new(size, Instant::now() + Duration::from_secs(30)).unwrap();
        assert!(check.observe(crate::api::Point { x: 0.0, y: 0.0 }).is_err());
        assert!(check.target().is_err());
        let mut check = TargetCheck::new(size, Instant::now() + Duration::from_secs(30)).unwrap();
        check.cancel();
        assert!(check.target().is_err());
        assert!(TargetCheck::new(size, Instant::now()).is_err());
    }
}

/// Locate the deliberately rendered solid marker in an owned test screenshot.
/// Multiple/disjoint markers, absent markers and malformed PNGs fail closed.
/// This is a synthetic calibration aid, not arbitrary UI/control recognition.
pub fn synthetic_marker(png: &[u8], completed: bool) -> Result<crate::api::Point, ErrorCode> {
    use image::ImageDecoder;
    if png.len() > 8 * 1024 * 1024 {
        return Err(ErrorCode::OutputLimit);
    }
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(32 * 1024 * 1024);
    limits.max_image_width = Some(4096);
    limits.max_image_height = Some(4096);
    let decoder = image::codecs::png::PngDecoder::with_limits(std::io::Cursor::new(png), limits)
        .map_err(|_| ErrorCode::CaptureUnavailable)?;
    if decoder.total_bytes() > 32 * 1024 * 1024 {
        return Err(ErrorCode::OutputLimit);
    }
    let image = image::DynamicImage::from_decoder(decoder)
        .map_err(|_| ErrorCode::CaptureUnavailable)?
        .to_rgb8();
    let color = if completed {
        [0, 211, 0]
    } else {
        [211, 0, 211]
    };
    let (mut left, mut top, mut right, mut bottom, mut count) = (u32::MAX, u32::MAX, 0, 0, 0u64);
    for (x, y, pixel) in image.enumerate_pixels() {
        if pixel.0 == color {
            left = left.min(x);
            top = top.min(y);
            right = right.max(x);
            bottom = bottom.max(y);
            count += 1;
        }
    }
    if count == 0 {
        return Err(ErrorCode::MappingUnverified);
    }
    let (width, height) = (right - left + 1, bottom - top + 1);
    if !(8..=256).contains(&width)
        || !(8..=256).contains(&height)
        || width.abs_diff(height) > 2
        || (top + 1..bottom).any(|y| (left + 1..right).any(|x| image.get_pixel(x, y).0 != color))
    {
        return Err(ErrorCode::MappingUnverified);
    }
    Ok(crate::api::Point {
        x: f64::from(left + right) / 2.0,
        y: f64::from(top + bottom) / 2.0,
    })
}

#[cfg(test)]
mod marker_tests {
    use super::*;
    fn encode(image: image::RgbImage) -> Vec<u8> {
        let mut out = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(image)
            .write_to(&mut out, image::ImageFormat::Png)
            .unwrap();
        out.into_inner()
    }
    #[test]
    fn marker_coordinates_are_image_pixels_and_ambiguity_fails_closed() {
        let mut image = image::RgbImage::new(200, 400);
        for y in 100..120 {
            for x in 80..100 {
                image.put_pixel(x, y, image::Rgb([211, 0, 211]));
            }
        }
        let point = synthetic_marker(&encode(image.clone()), false).unwrap();
        assert_eq!((point.x, point.y), (89.5, 109.5));
        assert!(synthetic_marker(&encode(image.clone()), true).is_err());
        // Fractional CSS placement can antialias only part of the outer row.
        for x in 80..90 {
            image.put_pixel(x, 100, image::Rgb([0, 0, 0]));
        }
        let point = synthetic_marker(&encode(image.clone()), false).unwrap();
        assert_eq!((point.x, point.y), (89.5, 109.5));
        let mut hole = image.clone();
        hole.put_pixel(90, 110, image::Rgb([0, 0, 0]));
        assert!(synthetic_marker(&encode(hole), false).is_err());
        image.put_pixel(150, 200, image::Rgb([211, 0, 211]));
        assert!(synthetic_marker(&encode(image), false).is_err());
        assert!(synthetic_marker(b"invalid png", false).is_err());
    }
}
