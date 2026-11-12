//! Bounded memory-only observations and trusted mapping profiles. No calibration claim.
use crate::{api::*, geometry::Transform};
use std::collections::VecDeque;

#[derive(Clone)]
pub struct Limits {
    pub devices: usize,
    pub frames: usize,
    pub frame_bytes: usize,
    pub receipts: usize,
    pub operation_ms: u64,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            devices: 16,
            frames: 4,
            frame_bytes: 8 * 1024 * 1024,
            receipts: 16,
            operation_ms: 1500,
        }
    }
}
impl Limits {
    pub fn validate(&self) -> Result<(), ErrorCode> {
        if !(1..=16).contains(&self.devices)
            || !(1..=16).contains(&self.frames)
            || !(1024..=32 * 1024 * 1024).contains(&self.frame_bytes)
            || !(1..=256).contains(&self.receipts)
            || !(100..=1500).contains(&self.operation_ms)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        Ok(())
    }
}
/// Produced by trusted setup, never accepted in an IPC request. Matching metadata
/// is necessary but cannot manufacture calibration or measured hardware readiness.
#[derive(Clone, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Profile {
    pub build: String,
    pub descriptor: String,
    pub backend: String,
    pub display: Size,
    pub rotation: Rotation,
    pub geometry: GeometryVersion,
    pub mapping: MappingVersion,
    pub transform: Transform,
}
impl Profile {
    pub fn validate(
        &self,
        build: &str,
        descriptor: &str,
        backend: &str,
        display: Size,
        rotation: Rotation,
    ) -> Result<(), ErrorCode> {
        if self.build.is_empty()
            || self.descriptor.is_empty()
            || self.backend.is_empty()
            || self.build.len() > 256
            || self.descriptor.len() > 128
            || self.backend.len() > 64
            || self.build != build
            || self.descriptor != descriptor
            || self.backend != backend
            || self.display != display
            || self.rotation != rotation
            || self.transform.rotation != rotation
            || self.geometry.0 == 0
            || self.mapping.0 == 0
        {
            return Err(ErrorCode::GeometryChanged);
        }
        if self.transform.usable() {
            self.transform.map(display, Point { x: 0., y: 0. })?;
        }
        Ok(())
    }
}
/// Proof of full decode and exact image metadata. Fields cannot be forged by
/// the dispatcher or wire callers; final admission rechecks age and capacity.
pub(crate) struct PreparedFrame {
    frame: Frame,
    mapping: Transform,
    png: Vec<u8>,
}
impl PreparedFrame {
    pub(crate) fn frame(&self) -> &Frame {
        &self.frame
    }
    pub(crate) fn png(&self) -> &[u8] {
        &self.png
    }
    /// Trusted geometry-only UI observation, never an image. Legacy screenshot
    /// input cannot consume this screen-coordinate token.
    pub(crate) fn geometry(frame: Frame, mapping: Transform, now: u64) -> Result<Self, ErrorCode> {
        if frame.coordinate_space != CoordinateSpace::ScreenPixels
            || frame.image != frame.display
            || frame.display.width < 2
            || frame.display.height < 2
            || frame.id.0.is_empty()
            || frame.display_id != 0
            || now
                .checked_sub(frame.captured_ms)
                .is_none_or(|age| age > 30_000)
        {
            return Err(ErrorCode::GeometryChanged);
        }
        Ok(Self {
            frame,
            mapping,
            png: vec![],
        })
    }
    pub(crate) fn prepare(
        frame: Frame,
        mapping: Transform,
        png: Vec<u8>,
        now: u64,
        limit: usize,
    ) -> Result<Self, ErrorCode> {
        if frame.id.0.is_empty()
            || frame.id.0.len() > 128
            || frame.display_id != 0
            || now
                .checked_sub(frame.captured_ms)
                .is_none_or(|age| age > 30_000)
        {
            return Err(ErrorCode::FrameExpired);
        }
        if png.len() > limit {
            return Err(ErrorCode::OutputLimit);
        }
        if mapping.usable() {
            mapping.map(frame.image, Point { x: 0., y: 0. })?;
        }
        let dimensions =
            image::ImageReader::with_format(std::io::Cursor::new(&png), image::ImageFormat::Png)
                .into_dimensions()
                .map_err(|_| ErrorCode::CaptureUnavailable)?;
        if dimensions != (frame.image.width, frame.image.height) {
            return Err(ErrorCode::GeometryChanged);
        }
        let (_, size) = crate::adb::resize_png(&png, frame.image.width.max(frame.image.height))?;
        if size != frame.image {
            return Err(ErrorCode::GeometryChanged);
        }
        Ok(Self {
            frame,
            mapping,
            png,
        })
    }
}
pub struct FrameCache {
    limits: Limits,
    bytes: usize,
    frames: VecDeque<(Frame, Transform, Vec<u8>)>,
}
impl FrameCache {
    pub fn new(limits: Limits) -> Result<Self, ErrorCode> {
        limits.validate()?;
        Ok(Self {
            limits,
            bytes: 0,
            frames: VecDeque::new(),
        })
    }
    pub fn insert(
        &mut self,
        frame: Frame,
        mapping: Transform,
        png: Vec<u8>,
        now: u64,
    ) -> Result<(), ErrorCode> {
        let prepared = PreparedFrame::prepare(frame, mapping, png, now, self.limits.frame_bytes)?;
        self.insert_prepared(prepared, now)
    }
    pub(crate) fn insert_prepared(
        &mut self,
        prepared: PreparedFrame,
        now: u64,
    ) -> Result<(), ErrorCode> {
        let PreparedFrame {
            frame,
            mapping,
            png,
        } = prepared;
        if now
            .checked_sub(frame.captured_ms)
            .is_none_or(|age| age > 30_000)
            || self.frames.iter().any(|(f, _, _)| f.id == frame.id)
        {
            return Err(ErrorCode::FrameExpired);
        }
        if png.len() > self.limits.frame_bytes {
            return Err(ErrorCode::OutputLimit);
        }
        while self.frames.len() >= self.limits.frames
            || self.bytes + png.len() > self.limits.frame_bytes
        {
            let (_, _, old) = self.frames.pop_front().ok_or(ErrorCode::OutputLimit)?;
            self.bytes -= old.len();
        }
        self.bytes += png.len();
        self.frames.push_back((frame, mapping, png));
        Ok(())
    }
    pub fn get(
        &self,
        auth: &LeaseAuth,
        id: &FrameId,
        now: u64,
    ) -> Result<(&Frame, &Transform, &[u8]), ErrorCode> {
        let (f, m, p) = self
            .frames
            .iter()
            .find(|(f, _, _)| &f.id == id)
            .ok_or(ErrorCode::FrameExpired)?;
        let v = &f.provenance;
        if v.boot_id != auth.boot_id
            || v.device_id != auth.device_id
            || v.lease_id != auth.lease_id
            || v.attachment != auth.attachment
            || v.fence != auth.fence
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        if now
            .checked_sub(f.captured_ms)
            .is_none_or(|age| age > 30_000)
        {
            return Err(ErrorCode::FrameExpired);
        }
        Ok((f, m, p))
    }
    pub fn bytes(&self) -> usize {
        self.bytes
    }
    pub fn clear(&mut self) {
        self.frames.clear();
        self.bytes = 0;
    }
}
