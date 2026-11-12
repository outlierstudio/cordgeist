//! Narrow AOSP DisplayViewport::toString adapter (schema aosp-viewport-v1).
//! Unknown/OEM layouts are rejected; no orientation inference from dimensions.
use crate::api::{ErrorCode, Rotation, Size};
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DisplayMetadata {
    pub display_id: u32,
    pub unique_id: String,
    pub rotation: Rotation,
    pub logical: Size,
    pub physical: [u32; 4],
    pub device: Size,
    pub user: u32,
}
fn value<'a>(text: &'a str, key: &str) -> Result<&'a str, ErrorCode> {
    let mut matches = text.match_indices(key);
    let start = matches.next().ok_or(ErrorCode::MetadataUnsupported)?.0 + key.len();
    if matches.next().is_some() {
        return Err(ErrorCode::MetadataUnsupported);
    }
    Ok(&text[start..])
}
fn number(text: &str) -> Result<u32, ErrorCode> {
    text.parse().map_err(|_| ErrorCode::MetadataUnsupported)
}
fn rect<const N: usize>(text: &str, key: &str) -> Result<[u32; N], ErrorCode> {
    let rest = value(text, key)?;
    let rest = rest
        .strip_prefix('[')
        .ok_or(ErrorCode::MetadataUnsupported)?;
    let rest = rest
        .split_once(']')
        .ok_or(ErrorCode::MetadataUnsupported)?
        .0;
    let fields = rest
        .split(", ")
        .map(number)
        .collect::<Result<Vec<_>, _>>()?;
    fields
        .try_into()
        .map_err(|_| ErrorCode::MetadataUnsupported)
}
pub fn parse_aosp_viewport(dump: &[u8], user: &[u8]) -> Result<DisplayMetadata, ErrorCode> {
    if dump.len() > 1024 * 1024 || user.len() > 32 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let dump = std::str::from_utf8(dump).map_err(|_| ErrorCode::MetadataUnsupported)?;
    let user = number(
        std::str::from_utf8(user)
            .map_err(|_| ErrorCode::MetadataUnsupported)?
            .trim(),
    )?;
    // Initial personal-user path only. Secondary/work users are not assumed safe.
    if user != 0 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let mut viewports = dump
        .lines()
        .filter(|line| {
            line.trim_start()
                .starts_with("Viewport INTERNAL: displayId=0, ")
        })
        .map(str::trim)
        .collect::<Vec<_>>();
    viewports.sort_unstable();
    viewports.dedup();
    if viewports.len() != 1 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let line = viewports[0];
    if line.contains("isActive=") && !line.ends_with("isActive=[1]") {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let unique_id = value(line, "uniqueId=")?
        .split_once(", port=")
        .ok_or(ErrorCode::MetadataUnsupported)?
        .0;
    if unique_id.is_empty() {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let rotation = match value(line, "orientation=")?
        .split_once(", logicalFrame=")
        .ok_or(ErrorCode::MetadataUnsupported)?
        .0
    {
        "0" => Rotation::R0,
        "1" => Rotation::R90,
        "2" => Rotation::R180,
        "3" => Rotation::R270,
        _ => return Err(ErrorCode::MetadataUnsupported),
    };
    let logical = rect::<4>(line, "logicalFrame=")?;
    let physical = rect::<4>(line, "physicalFrame=")?;
    let device = rect::<2>(line, "deviceSize=")?;
    if logical[0] != 0
        || logical[1] != 0
        || logical[2] < 2
        || logical[3] < 2
        || physical[2] <= physical[0]
        || physical[3] <= physical[1]
        || device[0] < 2
        || device[1] < 2
    {
        return Err(ErrorCode::MetadataUnsupported);
    }
    Ok(DisplayMetadata {
        display_id: 0,
        unique_id: unique_id.into(),
        rotation,
        logical: Size {
            width: logical[2],
            height: logical[3],
        },
        physical,
        device: Size {
            width: device[0],
            height: device[1],
        },
        user,
    })
}

/// Default-display fallback needs a single unambiguous rotation and personal user.
/// Unknown/contradictory metadata still fails; it never guesses an orientation.
pub fn generic_viewport(
    dump: &[u8],
    user: &[u8],
    physical: Size,
) -> Result<DisplayMetadata, ErrorCode> {
    if dump.len() > 1024 * 1024
        || user.len() > 32
        || user
            .iter()
            .copied()
            .filter(|b| !b.is_ascii_whitespace())
            .collect::<Vec<_>>()
            != b"0"
    {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let text = std::str::from_utf8(dump).map_err(|_| ErrorCode::MetadataUnsupported)?;
    let values = text
        .lines()
        .filter_map(|line| line.trim().strip_prefix("SurfaceOrientation:"))
        .map(str::trim)
        .collect::<Vec<_>>();
    if values.len() != 1 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let rotation = match values[0] {
        "0" => Rotation::R0,
        "1" => Rotation::R90,
        "2" => Rotation::R180,
        "3" => Rotation::R270,
        _ => return Err(ErrorCode::MetadataUnsupported),
    };
    let logical = if matches!(rotation, Rotation::R90 | Rotation::R270) {
        Size {
            width: physical.height,
            height: physical.width,
        }
    } else {
        physical
    };
    if physical.width < 2 || physical.height < 2 {
        return Err(ErrorCode::MetadataUnsupported);
    }
    Ok(DisplayMetadata {
        display_id: 0,
        unique_id: "default-display".into(),
        rotation,
        logical,
        physical: [0, 0, physical.width, physical.height],
        device: physical,
        user: 0,
    })
}
#[cfg(test)]
mod generic_tests {
    use super::*;
    #[test]
    fn fallback_requires_exact_rotation_and_user() {
        let s = Size {
            width: 1080,
            height: 2220,
        };
        assert_eq!(
            generic_viewport(b"SurfaceOrientation: 1\n", b"0\n", s)
                .unwrap()
                .logical,
            Size {
                width: 2220,
                height: 1080
            }
        );
        assert!(
            generic_viewport(b"SurfaceOrientation: 0\nSurfaceOrientation: 1", b"0", s).is_err()
        );
        assert!(generic_viewport(b"SurfaceOrientation: 0", b"10", s).is_err());
    }
}
