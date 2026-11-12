use crate::{api::*, backend::*, core::Controller, geometry::Transform, testkit::*};
fn fixture(
    limit: usize,
) -> (
    Controller<ScriptedInput, FakeClock>,
    ActionRequest,
    FakeClock,
) {
    let clock = FakeClock::default();
    let auth = LeaseAuth {
        boot_id: BootId("boot".into()),
        device_id: DeviceId("phone".into()),
        lease_id: LeaseId("lease".into()),
        credential: Credential("private".into()),
        attachment: AttachmentGeneration(1),
        fence: Fence(1),
    };
    let provenance = Provenance {
        boot_id: auth.boot_id.clone(),
        device_id: auth.device_id.clone(),
        lease_id: auth.lease_id.clone(),
        attachment: auth.attachment,
        fence: auth.fence,
        geometry: GeometryVersion(1),
        mapping: MappingVersion(1),
    };
    let frame = Frame {
        id: FrameId("f1".into()),
        provenance: provenance.clone(),
        captured_ms: 0,
        image: Size {
            width: 100,
            height: 200,
        },
        display: Size {
            width: 1000,
            height: 2000,
        },
        rotation: Rotation::R0,
        display_id: 0,
        coordinate_space: CoordinateSpace::ReturnedImagePixels,
    };
    let mut c = Controller::new(
        ScriptedInput::new([Fault::Completed; 10]),
        clock.clone(),
        auth.clone(),
        provenance,
        600_000,
        limit,
    )
    .unwrap();
    c.retain_frame(
        frame,
        Transform {
            rotation: Rotation::R0,
            max_x: 32767,
            max_y: 32767,
            verified: true,
            generic_screen: None,
        },
    )
    .unwrap();
    let req = ActionRequest {
        auth,
        seq: ActionSeq(1),
        frame_id: FrameId("f1".into()),
        input: Input::Tap {
            point: Point { x: 1.0, y: 2.0 },
        },
    };
    (c, req, clock)
}
#[test]
fn invalid_input_and_stale_provenance_send_nothing() {
    for kind in 0..12 {
        let (mut c, mut req, clock) = fixture(2);
        match kind {
            0 => {
                req.input = Input::Tap {
                    point: Point { x: -1.0, y: 0.0 },
                }
            }
            1 => {
                req.input = Input::Tap {
                    point: Point { x: 100.0, y: 0.0 },
                }
            }
            2 => {
                req.input = Input::Tap {
                    point: Point {
                        x: f64::NAN,
                        y: 0.0,
                    },
                }
            }
            3 => {
                req.input = Input::Type {
                    text: "valid then 😀".into(),
                }
            }
            4 => req.auth.credential = Credential("wrong".into()),
            5 => req.auth.boot_id = BootId("old".into()),
            6 => req.auth.attachment = AttachmentGeneration(0),
            7 => req.auth.device_id = DeviceId("other".into()),
            8 => req.auth.fence = Fence(0),
            9 => clock.advance(30_001),
            10 => {
                req.input = Input::Swipe {
                    from: Point { x: 0.0, y: 0.0 },
                    to: Point { x: 1.0, y: 1.0 },
                    duration_ms: 10_001,
                }
            }
            _ => {
                req.input = Input::Key {
                    key: "unverified-navigation".into(),
                }
            }
        }
        assert!(c.act(req, &Cancel::default()).is_err(), "case {kind}");
        assert_eq!(c.backend().effects, 0, "case {kind}");
    }
}
#[test]
fn duplicate_precedes_frame_validation_and_eviction_cannot_replay() {
    let (mut c, req, clock) = fixture(1);
    let receipt = c.act(req.clone(), &Cancel::default()).unwrap();
    clock.advance(30_001);
    assert_eq!(
        c.act(req.clone(), &Cancel::default()).unwrap().action_id,
        receipt.action_id
    );
    assert_eq!(c.backend().effects, 1);
    let mut other = req.clone();
    other.input = Input::Type {
        text: "changed".into(),
    };
    assert_eq!(
        c.act(other, &Cancel::default()).unwrap_err(),
        ErrorCode::SequenceMismatch
    );
    // New fixture permits eviction with a fresh frame.
    let (mut c, req, _) = fixture(1);
    c.act(req.clone(), &Cancel::default()).unwrap();
    let mut next = req.clone();
    next.seq = ActionSeq(2);
    c.act(next, &Cancel::default()).unwrap();
    assert_eq!(
        c.act(req, &Cancel::default()).unwrap_err(),
        ErrorCode::ReceiptExpired
    );
    assert_eq!(c.backend().effects, 2);
}
#[test]
fn revoke_expiry_and_geometry_change_fence_backend() {
    let (mut c, req, _) = fixture(1);
    assert_eq!(c.revoke(), Cleanup::Neutral);
    assert_eq!(
        c.act(req, &Cancel::default()).unwrap_err(),
        ErrorCode::LeaseInvalid
    );
    assert_eq!(c.backend().effects, 0);
    let (mut c, req, clock) = fixture(1);
    clock.advance(600_000);
    assert_eq!(
        c.act(req, &Cancel::default()).unwrap_err(),
        ErrorCode::LeaseExpired
    );
    assert_eq!(c.backend().effects, 0);
    let (mut c, req, _) = fixture(1);
    let mut p = c_current(&req.auth);
    p.geometry = GeometryVersion(2);
    c.reconfigure(p);
    assert!(c.act(req, &Cancel::default()).is_err());
    assert_eq!(c.backend().effects, 0);
}
fn c_current(a: &LeaseAuth) -> Provenance {
    Provenance {
        boot_id: a.boot_id.clone(),
        device_id: a.device_id.clone(),
        lease_id: a.lease_id.clone(),
        attachment: a.attachment,
        fence: a.fence,
        geometry: GeometryVersion(1),
        mapping: MappingVersion(1),
    }
}
#[test]
fn uncertain_and_pending_outcomes_quarantine_without_replay() {
    for fault in [Fault::Partial, Fault::Disconnect, Fault::CleanupPending] {
        let (_, req, clock) = fixture(1);
        let mut c = Controller::new(
            ScriptedInput::new([fault]),
            clock,
            req.auth.clone(),
            c_current(&req.auth),
            600_000,
            1,
        )
        .unwrap();
        // Install synthetic frame in this newly constructed fake controller.
        c.retain_frame(
            Frame {
                id: req.frame_id.clone(),
                provenance: c_current(&req.auth),
                captured_ms: 0,
                image: Size {
                    width: 100,
                    height: 200,
                },
                display: Size {
                    width: 100,
                    height: 200,
                },
                rotation: Rotation::R0,
                display_id: 0,
                coordinate_space: CoordinateSpace::ReturnedImagePixels,
            },
            Transform {
                rotation: Rotation::R0,
                max_x: 32767,
                max_y: 32767,
                verified: true,
                generic_screen: None,
            },
        )
        .unwrap();
        let receipt = c.act(req.clone(), &Cancel::default()).unwrap();
        assert_ne!(receipt.input_status, InputStatus::NotSent);
        c.act(req.clone(), &Cancel::default()).unwrap();
        let mut next = req;
        next.seq = ActionSeq(2);
        assert_eq!(
            c.act(next, &Cancel::default()).unwrap_err(),
            ErrorCode::NativeCleanupPending
        );
        assert_eq!(c.backend().effects, 1);
    }
}
#[test]
fn transform_endpoints_and_fractional_rotation() {
    let size = Size {
        width: 101,
        height: 201,
    };
    for (rotation, corners) in [
        (Rotation::R0, [(0, 0), (1000, 0), (0, 2000), (1000, 2000)]),
        (Rotation::R90, [(0, 2000), (0, 0), (1000, 2000), (1000, 0)]),
        (Rotation::R180, [(1000, 2000), (0, 2000), (1000, 0), (0, 0)]),
        (Rotation::R270, [(1000, 0), (1000, 2000), (0, 0), (0, 2000)]),
    ] {
        let t = Transform {
            rotation,
            max_x: 1000,
            max_y: 2000,
            verified: true,
            generic_screen: None,
        };
        for (p, expected) in [
            Point { x: 0.0, y: 0.0 },
            Point { x: 100.0, y: 0.0 },
            Point { x: 0.0, y: 200.0 },
            Point { x: 100.0, y: 200.0 },
        ]
        .into_iter()
        .zip(corners)
        {
            assert_eq!(t.map(size, p).unwrap(), expected);
        }
        assert_eq!(
            t.map(size, Point { x: 50.0, y: 100.0 }).unwrap(),
            (500, 1000)
        );
    }
}
#[test]
fn catalogue_matches_public_api_and_effect_requirements() {
    let api = include_str!("../docs/api.md");
    let mut names = std::collections::HashSet::new();
    for op in OPERATIONS {
        assert!(names.insert(op.method));
        assert!(api.contains(&format!("`{}`", op.method)));
        if matches!(op.effect, Effect::Input) {
            assert!(op.lease);
            // App launch and fixed buttons have route/authority fencing but no
            // pixel target; every coordinate input still requires a frame.
            assert!(op.frame || op.method == "input.android");
        }
    }
    assert_eq!(OPERATIONS.len(), 20);
    let setup = OPERATIONS
        .iter()
        .find(|op| op.method == "setup.calibrate")
        .unwrap();
    assert!(!setup.mcp && !setup.lease && !setup.frame);
    assert!(matches!(setup.effect, Effect::Control));
}
#[test]
fn boot_or_lease_reconfiguration_cannot_reuse_previous_credentials() {
    for change in 0..3 {
        let (mut c, req, _) = fixture(1);
        let mut p = c_current(&req.auth);
        match change {
            0 => p.boot_id = BootId("new".into()),
            1 => p.device_id = DeviceId("other".into()),
            _ => p.lease_id = LeaseId("new".into()),
        };
        c.reconfigure(p);
        assert_eq!(
            c.act(req, &Cancel::default()).unwrap_err(),
            ErrorCode::LeaseInvalid
        );
        assert_eq!(c.backend().effects, 0);
    }
}
#[test]
fn cancelled_preflight_and_presend_failure_are_not_delivery() {
    let (mut c, req, _) = fixture(1);
    let cancel = Cancel::default();
    cancel.cancel();
    assert_eq!(
        c.act(req.clone(), &cancel).unwrap_err(),
        ErrorCode::ProcessCancelled
    );
    assert_eq!(c.backend().effects, 0);
    // Validation/cancellation rejection does not reserve the sequence.
    assert_eq!(
        c.act(req, &Cancel::default()).unwrap().input_status,
        InputStatus::Sent
    );
    let (_, req, clock) = fixture(1);
    let mut c = Controller::new(
        ScriptedInput::new([Fault::BeforeSend]),
        clock,
        req.auth.clone(),
        c_current(&req.auth),
        600_000,
        1,
    )
    .unwrap();
    c.retain_frame(
        Frame {
            id: req.frame_id.clone(),
            provenance: c_current(&req.auth),
            captured_ms: 0,
            image: Size {
                width: 100,
                height: 200,
            },
            display: Size {
                width: 100,
                height: 200,
            },
            rotation: Rotation::R0,
            display_id: 0,
            coordinate_space: CoordinateSpace::ReturnedImagePixels,
        },
        Transform {
            rotation: Rotation::R0,
            max_x: 32767,
            max_y: 32767,
            verified: true,
            generic_screen: None,
        },
    )
    .unwrap();
    let receipt = c.act(req.clone(), &Cancel::default()).unwrap();
    assert_eq!(receipt.input_status, InputStatus::NotSent);
    c.act(req, &Cancel::default()).unwrap();
    assert_eq!(c.backend().effects, 0);
}
#[test]
fn protocol_catalogue_has_exact_request_schema_coverage() {
    let schema = serde_json::to_value(schemars::schema_for!(Call)).unwrap();
    let variants = schema["oneOf"].as_array().unwrap();
    let names = variants
        .iter()
        .map(|v| v["properties"]["method"]["const"].as_str().unwrap())
        .collect::<std::collections::HashSet<_>>();
    assert_eq!(names.len(), OPERATIONS.len());
    for op in OPERATIONS {
        assert!(names.contains(op.method));
    }
}

#[test]
fn amended_attachment_or_fence_cannot_upgrade_original_credential() {
    for change in 0..3 {
        let (mut c, mut req, _) = fixture(1);
        let original = c_current(&req.auth);
        let mut current = original.clone();
        if change != 1 {
            current.attachment = AttachmentGeneration(2);
        }
        if change != 0 {
            current.fence = Fence(2);
        }
        c.reconfigure(current.clone());
        c.retain_frame(
            Frame {
                id: FrameId("fresh".into()),
                provenance: current.clone(),
                captured_ms: 0,
                image: Size {
                    width: 100,
                    height: 200,
                },
                display: Size {
                    width: 100,
                    height: 200,
                },
                rotation: Rotation::R0,
                display_id: 0,
                coordinate_space: CoordinateSpace::ReturnedImagePixels,
            },
            Transform {
                rotation: Rotation::R0,
                max_x: 32767,
                max_y: 32767,
                verified: true,
                generic_screen: None,
            },
        )
        .unwrap();
        req.frame_id = FrameId("fresh".into());
        req.auth.attachment = current.attachment;
        req.auth.fence = current.fence;
        assert_eq!(
            c.act(req.clone(), &Cancel::default()).unwrap_err(),
            ErrorCode::LeaseInvalid
        );
        assert_eq!(c.backend().effects, 0);
        c.reconfigure(original);
        req.auth.attachment = AttachmentGeneration(1);
        req.auth.fence = Fence(1);
        assert_eq!(
            c.act(req, &Cancel::default()).unwrap_err(),
            ErrorCode::LeaseInvalid
        );
        assert_eq!(c.backend().effects, 0);
    }
}

// Software clock fault: advances monotonically on the final admission timestamp,
// after the initial authorization and frame-age reads. No controller state is patched.
#[derive(Default)]
struct AdmissionClock {
    now: std::cell::Cell<u64>,
    reads: std::cell::Cell<Option<u32>>,
    jump_to: std::cell::Cell<u64>,
}
impl Clock for std::rc::Rc<AdmissionClock> {
    fn now_ms(&self) -> u64 {
        if let Some(n) = self.reads.get() {
            if n == 1 {
                self.now.set(self.jump_to.get());
                self.reads.set(None);
            } else {
                self.reads.set(Some(n - 1));
            }
        }
        self.now.get()
    }
}
#[test]
fn time_advancing_during_admission_rejects_before_effect_or_reservation() {
    for (jump, expected) in [
        (30_001, ErrorCode::FrameExpired),
        (600_000, ErrorCode::LeaseExpired),
    ] {
        let (_, mut req, _) = fixture(1);
        let clock = std::rc::Rc::new(AdmissionClock::default());
        let mut c = Controller::new(
            ScriptedInput::new([Fault::Completed]),
            clock.clone(),
            req.auth.clone(),
            c_current(&req.auth),
            600_000,
            1,
        )
        .unwrap();
        let fresh = |id: &str, captured_ms| Frame {
            id: FrameId(id.into()),
            provenance: c_current(&req.auth),
            captured_ms,
            image: Size {
                width: 100,
                height: 200,
            },
            display: Size {
                width: 100,
                height: 200,
            },
            rotation: Rotation::R0,
            display_id: 0,
            coordinate_space: CoordinateSpace::ReturnedImagePixels,
        };
        let mapping = Transform {
            rotation: Rotation::R0,
            max_x: 32767,
            max_y: 32767,
            verified: true,
            generic_screen: None,
        };
        c.retain_frame(fresh("f1", 0), mapping.clone()).unwrap();
        clock.jump_to.set(jump);
        clock.reads.set(Some(3));
        assert_eq!(
            c.act(req.clone(), &Cancel::default()).unwrap_err(),
            expected
        );
        assert_eq!(c.backend().effects, 0);
        assert_eq!(c.admitted_sequence(), 0);
        if expected == ErrorCode::FrameExpired {
            c.retain_frame(fresh("refreshed", jump), mapping).unwrap();
            req.frame_id = FrameId("refreshed".into());
            assert_eq!(c.act(req, &Cancel::default()).unwrap().seq, ActionSeq(1));
            assert_eq!(c.backend().effects, 1); // same sequence was not reserved
        }
    }
}
