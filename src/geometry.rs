use crate::api::*;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Explicit calibrated image-to-HID orientation; never inferred from image aspect.
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Transform {
    pub rotation: Rotation,
    pub max_x: u16,
    pub max_y: u16,
    pub verified: bool,
    /// Daemon-owned effective wm size; absent for legacy calibrated profiles.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub generic_screen: Option<Size>,
}
impl Transform {
    pub fn usable(&self) -> bool {
        self.verified || self.generic_screen.is_some()
    }

    pub fn map(&self, size: Size, p: Point) -> Result<(u16, u16), ErrorCode> {
        if let Some(physical) = self.generic_screen {
            return ScreenGeometry {
                physical,
                shot: size,
                rotation: self.rotation,
            }
            .map(p, self.max_x, self.max_y);
        }
        if !self.verified {
            return Err(ErrorCode::MappingUnverified);
        }
        self.place(size, p)
    }
    /// Pixel-to-descriptor placement. This does not mark the mapping verified.
    pub fn place(&self, size: Size, p: Point) -> Result<(u16, u16), ErrorCode> {
        if size.width < 2
            || size.height < 2
            || self.max_x == 0
            || self.max_y == 0
            || !p.x.is_finite()
            || !p.y.is_finite()
            || p.x < 0.0
            || p.y < 0.0
            || p.x > f64::from(size.width - 1)
            || p.y > f64::from(size.height - 1)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let x = p.x / f64::from(size.width - 1);
        let y = p.y / f64::from(size.height - 1);
        let (x, y) = match self.rotation {
            Rotation::R0 => (x, y),
            Rotation::R90 => (y, 1.0 - x),
            Rotation::R180 => (1.0 - x, 1.0 - y),
            Rotation::R270 => (1.0 - y, x),
        };
        Ok((
            (x * f64::from(self.max_x)).round() as u16,
            (y * f64::from(self.max_y)).round() as u16,
        ))
    }
}

/// Effective display size, returned image size, and observed rotation. Rotation
/// is diagnostic: generic external-touch orientation is experimental outside R0.
#[derive(Clone, Copy, Debug)]
pub struct ScreenGeometry {
    pub physical: Size,
    pub shot: Size,
    pub rotation: Rotation,
}
// Adapted from mobile-mcp (Apache-2.0) src/coordinate-mapping.ts,
// commit 3b51952bcd83955f3a743f731f5a9beaebecdab7. Rust validation/HID conversion added.
pub fn orient_like(mut physical: Size, shot: Size) -> Size {
    if (physical.width > physical.height) != (shot.width > shot.height) {
        std::mem::swap(&mut physical.width, &mut physical.height);
    }
    physical
}
impl ScreenGeometry {
    pub fn map(self, p: Point, max_x: u16, max_y: u16) -> Result<(u16, u16), ErrorCode> {
        let screen = orient_like(self.physical, self.shot);
        if screen.width < 2
            || screen.height < 2
            || self.shot.width == 0
            || self.shot.height == 0
            || max_x == 0
            || max_y == 0
            || !p.x.is_finite()
            || !p.y.is_finite()
            || p.x < 0.
            || p.y < 0.
            || p.x >= f64::from(self.shot.width)
            || p.y >= f64::from(self.shot.height)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let axis = |value: f64, pixels: u32, shot: u32, max: u16| {
            (value * f64::from(pixels) / f64::from(shot) * f64::from(max) / f64::from(pixels - 1))
                .round()
                .clamp(0., f64::from(max)) as u16
        };
        Ok((
            axis(p.x, screen.width, self.shot.width, max_x),
            axis(p.y, screen.height, self.shot.height, max_y),
        ))
    }
}
/// Parse effective wm size; conflicting duplicate lines and malformed overrides fail closed.
pub fn wm_size(bytes: &[u8]) -> Result<Size, ErrorCode> {
    if bytes.len() > 4096 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let text = std::str::from_utf8(bytes).map_err(|_| ErrorCode::MetadataUnsupported)?;
    let mut physical = None;
    let mut override_size = None;
    for line in text.lines().map(str::trim) {
        let (slot, rest) = if let Some(v) = line.strip_prefix("Physical size:") {
            (&mut physical, v)
        } else if let Some(v) = line.strip_prefix("Override size:") {
            (&mut override_size, v)
        } else {
            continue;
        };
        let (w, h) = rest
            .trim()
            .split_once('x')
            .ok_or(ErrorCode::MetadataUnsupported)?;
        let size = Size {
            width: w.parse().map_err(|_| ErrorCode::MetadataUnsupported)?,
            height: h.parse().map_err(|_| ErrorCode::MetadataUnsupported)?,
        };
        if slot.is_some()
            || size.width < 2
            || size.height < 2
            || size.width > 16384
            || size.height > 16384
        {
            return Err(ErrorCode::MetadataUnsupported);
        }
        *slot = Some(size);
    }
    override_size
        .or(physical)
        .ok_or(ErrorCode::MetadataUnsupported)
}
pub fn rotation_warnings(rotation: Rotation) -> Vec<String> {
    if rotation == Rotation::R0 {
        vec![]
    } else {
        vec!["landscape is experimental; taps may be rotated".into()]
    }
}

#[cfg(test)]
mod generic_tests {
    use super::*;
    fn size(width: u32, height: u32) -> Size {
        Size { width, height }
    }
    #[test]
    fn portrait_edges_downscale_and_single_pixel() {
        let g = ScreenGeometry {
            physical: size(1080, 2220),
            shot: size(1080, 2220),
            rotation: Rotation::R0,
        };
        assert_eq!(g.map(Point { x: 0., y: 0. }, 32767, 16383), Ok((0, 0)));
        assert_eq!(
            g.map(Point { x: 1079., y: 2219. }, 32767, 16383),
            Ok((32767, 16383))
        );
        assert!(g.map(Point { x: 1080., y: 0. }, 32767, 16383).is_err());
        let down = ScreenGeometry {
            shot: size(623, 1280),
            ..g
        };
        assert_eq!(
            down.map(Point { x: 311., y: 640. }, 32767, 32767),
            Ok((16372, 16391))
        );
        let one = ScreenGeometry {
            shot: size(1, 1),
            ..g
        };
        assert_eq!(one.map(Point { x: 0., y: 0. }, 32767, 32767), Ok((0, 0)));
    }
    #[test]
    fn landscape_and_override() {
        assert_eq!(
            orient_like(size(1080, 2220), size(1280, 623)),
            size(2220, 1080)
        );
        assert_eq!(
            wm_size(b"Physical size: 1440x2960\nOverride size: 1080x2220\n"),
            Ok(size(1080, 2220))
        );
        assert!(wm_size(b"Physical size: 1440x2960\nOverride size: bad").is_err());
        let g = ScreenGeometry {
            physical: size(1080, 2220),
            shot: size(2220, 1080),
            rotation: Rotation::R90,
        };
        assert_eq!(
            g.map(Point { x: 2219., y: 1079. }, 32767, 32767),
            Ok((32767, 32767))
        );
    }
}
