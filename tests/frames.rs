//! SOFTWARE-ONLY geometry/profile/cache tests; no calibration or phone proof.
use cordgeist::{api::*, frames::*, geometry::Transform};
fn auth() -> LeaseAuth {
    LeaseAuth {
        boot_id: BootId("boot".into()),
        device_id: DeviceId("device".into()),
        lease_id: LeaseId("lease".into()),
        credential: Credential("private".into()),
        attachment: AttachmentGeneration(1),
        fence: Fence(1),
    }
}
fn frame(id: &str) -> Frame {
    let a = auth();
    Frame {
        id: FrameId(id.into()),
        provenance: Provenance {
            boot_id: a.boot_id,
            device_id: a.device_id,
            lease_id: a.lease_id,
            attachment: a.attachment,
            fence: a.fence,
            geometry: GeometryVersion(1),
            mapping: MappingVersion(1),
        },
        captured_ms: 100,
        image: Size {
            width: 4,
            height: 8,
        },
        display: Size {
            width: 4,
            height: 8,
        },
        rotation: Rotation::R0,
        display_id: 0,
        coordinate_space: CoordinateSpace::ReturnedImagePixels,
    }
}
fn mapping() -> Transform {
    Transform {
        rotation: Rotation::R0,
        max_x: 32767,
        max_y: 32767,
        verified: true,
        generic_screen: None,
    }
}
#[test]
fn memory_eviction_age_provenance_and_identifier_reuse_fail_closed() {
    let mut cache = FrameCache::new(Limits {
        frames: 1,
        ..Limits::default()
    })
    .unwrap();
    let png = include_bytes!("fixtures/synthetic.png").to_vec();
    cache
        .insert(frame("a"), mapping(), png.clone(), 100)
        .unwrap();
    assert_eq!(cache.bytes(), png.len());
    assert_eq!(
        cache.insert(frame("a"), mapping(), png.clone(), 100),
        Err(ErrorCode::FrameExpired)
    );
    assert!(cache.get(&auth(), &FrameId("a".into()), 30101).is_err());
    let mut wrong = auth();
    wrong.fence = Fence(2);
    assert!(matches!(
        cache.get(&wrong, &FrameId("a".into()), 100),
        Err(ErrorCode::LeaseInvalid)
    ));
    cache
        .insert(frame("b"), mapping(), png.clone(), 100)
        .unwrap();
    assert!(cache.get(&auth(), &FrameId("a".into()), 100).is_err());
    assert_eq!(cache.bytes(), png.len());
    cache.clear();
    assert_eq!(cache.bytes(), 0);
}
#[test]
fn profile_metadata_and_configuration_are_not_caller_readiness() {
    let p = Profile {
        build: "synthetic-build".into(),
        descriptor: "descriptor".into(),
        backend: "software-fake".into(),
        display: Size {
            width: 4,
            height: 8,
        },
        rotation: Rotation::R0,
        geometry: GeometryVersion(1),
        mapping: MappingVersion(1),
        transform: mapping(),
    };
    p.validate(&p.build, &p.descriptor, &p.backend, p.display, p.rotation)
        .unwrap();
    assert_eq!(
        p.validate("other", &p.descriptor, &p.backend, p.display, p.rotation),
        Err(ErrorCode::GeometryChanged)
    );
    let mut unverified = p.clone();
    unverified.transform.verified = false;
    unverified
        .validate(&p.build, &p.descriptor, &p.backend, p.display, p.rotation)
        .unwrap();
    assert_eq!(
        unverified.transform.map(p.display, Point { x: 0., y: 0. }),
        Err(ErrorCode::MappingUnverified)
    );
    let mut cache = FrameCache::new(Limits::default()).unwrap();
    cache
        .insert(
            frame("unverified"),
            unverified.transform.clone(),
            include_bytes!("fixtures/synthetic.png").to_vec(),
            100,
        )
        .unwrap();
    for rotation in [Rotation::R0, Rotation::R90, Rotation::R180, Rotation::R270] {
        let mut m = mapping();
        m.rotation = rotation;
        assert!(
            m.map(
                Size {
                    width: 4,
                    height: 8
                },
                Point { x: 3., y: 7. }
            )
            .is_ok()
        );
        assert!(
            m.map(
                Size {
                    width: 4,
                    height: 8
                },
                Point { x: 4., y: 7. }
            )
            .is_err()
        );
    }
    assert!(
        Limits {
            receipts: 0,
            ..Limits::default()
        }
        .validate()
        .is_err()
    );
    assert!(
        Limits {
            operation_ms: 1501,
            ..Limits::default()
        }
        .validate()
        .is_err()
    );
}
#[test]
fn cache_rejects_png_that_only_matches_claimed_geometry_after_resizing() {
    let mut png = std::io::Cursor::new(Vec::new());
    image::RgbaImage::new(8, 16)
        .write_to(&mut png, image::ImageFormat::Png)
        .unwrap();
    let mut cache = FrameCache::new(Limits::default()).unwrap();
    assert_eq!(
        cache.insert(frame("wrong-source"), mapping(), png.into_inner(), 100),
        Err(ErrorCode::GeometryChanged)
    );
    assert_eq!(cache.bytes(), 0);
}
