//! Synthetic wire fixtures. No controller/device is instantiated by these tests.
use cordgeist::api::*;
use serde_json::{Value, json};
use std::collections::BTreeSet;
fn auth() -> Value {
    json!({"bootId":"synthetic-boot","deviceId":"synthetic-device","leaseId":"synthetic-lease",
        "credential":"SYNTHETIC_PRIVATE_CREDENTIAL","attachment":1,"fence":1})
}
fn frame() -> Value {
    json!({"id":"synthetic-frame","provenance":{"bootId":"synthetic-boot","deviceId":"synthetic-device",
        "leaseId":"synthetic-lease","attachment":1,"fence":1,"geometry":1,"mapping":1},
        "capturedMs":0,"image":{"width":4,"height":8},"display":{"width":4,"height":8},
        "rotation":"r0","displayId":0,"coordinateSpace":"returned-image-pixels"})
}
fn receipt() -> Value {
    json!({"actionId":"synthetic-action","seq":1,"transport":"FAKE-software-only",
        "startedMs":0,"finishedMs":1,"inputStatus":"unknown","observationStatus":"timeout",
        "cleanup":"pending","cancelled":false,"retryGuidance":"Query original sequence; do not resend"})
}
fn capabilities() -> Value {
    let cap = |e| json!({"evidence":e,"reason":"synthetic evidence only","profile":null});
    json!({"screenshot":cap("detected"),"aoaTouch":cap("unavailable"),"keyboard":cap("unavailable"),
        "supportedKeys":[],"keyboardLayout":null,"ocr":cap("verified"),"ocrLanguages":["eng"],"mediaScan":cap("unavailable")})
}
fn fixtures() -> Vec<(&'static str, Option<Value>, Value)> {
    let action = json!({"receipt":receipt(),"observation":null});
    let end = json!({"deviceId":"synthetic-device","leaseId":"synthetic-lease","fence":2,"cleanup":"pending"});
    let visual =
        json!({"auth":auth(),"seq":1,"frame_id":"synthetic-frame","point":{"x":0.0,"y":0.0}});
    vec![
        (
            "observations.elements",
            Some(json!({"auth":auth()})),
            json!({"frame":frame(),"elements":[],"warnings":[]}),
        ),
        (
            "apps.list",
            Some(json!({"auth":auth()})),
            json!({"apps":[{"packageName":"com.android.settings","appName":"com.android.settings"}]}),
        ),
        (
            "input.android",
            Some(json!({"auth":auth(),"seq":1,"command":{"kind":"press-button","button":"HOME"}})),
            action.clone(),
        ),
        (
            "setup.calibrate",
            Some(json!({"device":"synthetic-device"})),
            json!({"display":{"width":1080,"height":2220},"maximumX":32767,"maximumY":32767,"verified":false,"recovery":"Independent targets pending"}),
        ),
        (
            "system.status",
            None,
            json!({"softwareVersion":"synthetic","backend":"unavailable","ready":false,"capabilities":capabilities()}),
        ),
        (
            "system.doctor",
            Some(json!({"device":null})),
            json!({"deviceId":null,"checks":[{"code":"backend-unavailable","evidence":"unavailable","recovery":"configure deliberately"}],"capabilities":capabilities()}),
        ),
        (
            "devices.list",
            None,
            json!({"devices":[{"deviceId":"synthetic-device","attachment":1,"health":"quarantined","reasonCodes":["native-cleanup-pending"],"capabilities":capabilities(),"ownerLabel":null}]}),
        ),
        (
            "leases.acquire",
            Some(
                json!({"device":"synthetic-device","reason":"synthetic","label":null,"idle_ttl_s":600}),
            ),
            json!({"auth":auth(),"idleDeadlineMs":600000,"nextSeq":1}),
        ),
        ("leases.release", Some(json!({"auth":auth()})), end.clone()),
        (
            "leases.revoke",
            Some(json!({"device":"synthetic-device"})),
            end,
        ),
        (
            "observations.screenshot",
            Some(json!({"auth":auth(),"longest_edge":1280,"settle_ms":0})),
            json!({"frame":frame(),"image":{"mimeType":"image/png","base64":"SYNTHETIC_ENCODED_IMAGE"},"settled":null}),
        ),
        ("input.tap", Some(visual.clone()), action.clone()),
        (
            "input.swipe",
            Some(
                json!({"auth":auth(),"seq":1,"frame_id":"synthetic-frame","from":{"x":0.0,"y":0.0},"to":{"x":1.0,"y":1.0},"duration_ms":400}),
            ),
            action.clone(),
        ),
        (
            "input.hold",
            Some({
                let mut v = visual;
                v["duration_ms"] = json!(600);
                v
            }),
            action.clone(),
        ),
        (
            "input.type",
            Some(
                json!({"auth":auth(),"seq":1,"frame_id":"synthetic-frame","text":"SYNTHETIC_PRIVATE_INPUT"}),
            ),
            action.clone(),
        ),
        (
            "input.key",
            Some(json!({"auth":auth(),"seq":1,"frame_id":"synthetic-frame","key":"enter"})),
            action.clone(),
        ),
        (
            "actions.status",
            Some(json!({"auth":auth(),"seq":1})),
            json!({"phase":"final","action":action}),
        ),
        (
            "actions.cancel",
            Some(json!({"auth":auth(),"seq":1})),
            json!({"actionId":"synthetic-action","seq":1,"state":"requested","cleanup":"pending"}),
        ),
        (
            "observations.text",
            Some(json!({"auth":auth(),"frame_id":"synthetic-frame","language":"eng"})),
            json!({"frame":frame(),"language":"eng","words":[{"text":"SYNTHETIC_OCR_CONTENT","confidence":90.0,"origin":{"x":0.0,"y":0.0},"size":{"width":2,"height":2}}]}),
        ),
        (
            "media.push",
            Some(json!({"auth":auth(),"seq":1,"source":"/synthetic/source.png","kind":"image"})),
            json!({"receipt":receipt(),"media":{"copied":{"state":"known","detail":true},"indexed":{"state":"error","detail":"scan unavailable"},"pickerVerified":{"state":"unknown"},"destination":"/Pictures/Cordgeist/synthetic.png","bytes":140,"mime":"image/png","uri":null}}),
        ),
    ]
}
fn request(method: &str, params: Option<Value>) -> Request {
    let mut call = json!({"method":method});
    if let Some(p) = params {
        call["params"] = p;
    }
    serde_json::from_value(
        json!({"version":1,"requestId":"synthetic-request","deadlineMs":1000,"call":call}),
    )
    .unwrap()
}
fn schema_methods(schema: Value) -> BTreeSet<String> {
    let variants = schema["oneOf"].as_array().unwrap();
    assert_eq!(variants.len(), OPERATIONS.len());
    variants
        .iter()
        .map(|v| {
            v["properties"]["method"]["const"]
                .as_str()
                .unwrap()
                .to_owned()
        })
        .collect()
}
#[test]
fn catalogue_request_result_and_documentation_coverage_is_exhaustive() {
    let methods = OPERATIONS
        .iter()
        .map(|o| o.method.to_owned())
        .collect::<BTreeSet<_>>();
    assert_eq!(methods.len(), OPERATIONS.len());
    assert_eq!(
        methods,
        schema_methods(serde_json::to_value(schemars::schema_for!(Call)).unwrap())
    );
    assert_eq!(
        methods,
        schema_methods(serde_json::to_value(schemars::schema_for!(OperationResult)).unwrap())
    );
    assert_eq!(
        methods,
        fixtures()
            .into_iter()
            .map(|(m, _, _)| m.to_owned())
            .collect()
    );
    for m in methods {
        assert!(include_str!("../docs/api.md").contains(&format!("`{m}`")));
    }
}
#[test]
fn every_operation_roundtrips_a_correlated_typed_success_without_echoing_auth_or_input() {
    for (method, params, value) in fixtures() {
        let request = request(method, params);
        assert_eq!(request.call.method(), method);
        let result: OperationResult =
            serde_json::from_value(json!({"method":method,"value":value})).unwrap();
        assert_eq!(result.method(), method);
        let response = Response::for_request(
            &request,
            BootId("synthetic-boot".into()),
            ResponseOutcome::Success {
                result: Box::new(result),
            },
        )
        .unwrap();
        let bytes = serde_json::to_vec(&response).unwrap();
        let decoded: Response = serde_json::from_slice(&bytes).unwrap();
        assert!(
            decoded
                .validate_for(&request, &BootId("synthetic-boot".into()))
                .is_ok()
        );
        assert_eq!(
            serde_json::to_value(decoded).unwrap(),
            serde_json::from_slice::<Value>(&bytes).unwrap()
        );
        let wire = String::from_utf8(bytes).unwrap();
        assert!(!wire.contains("SYNTHETIC_PRIVATE_INPUT"));
        assert_eq!(
            wire.contains("SYNTHETIC_PRIVATE_CREDENTIAL"),
            method == "leases.acquire"
        );
    }
}
#[test]
fn envelope_rejects_conflicting_outcomes_unknown_versions_and_wrong_correlation() {
    let req = request("devices.list", None);
    let response = Response::for_request(
        &req,
        BootId("synthetic-boot".into()),
        ResponseOutcome::Success {
            result: Box::new(OperationResult::List(DevicesResult { devices: vec![] })),
        },
    )
    .unwrap();
    let wire = serde_json::to_value(response).unwrap();
    for version in [json!(0), json!(2), json!("1")] {
        let mut invalid = wire.clone();
        invalid["version"] = version;
        assert!(serde_json::from_value::<Response>(invalid).is_err());
    }
    for status in ["success", "error"] {
        let mut invalid = wire.clone();
        invalid["outcome"] = json!({"status":status,"result":{"method":"devices.list","value":{"devices":[]}},"error":{"code":"backend-unavailable","phase":"setup","recovery":"configure"}});
        assert!(serde_json::from_value::<Response>(invalid).is_err());
    }
    for missing in ["requestId", "bootId", "outcome"] {
        let mut invalid = wire.clone();
        invalid.as_object_mut().unwrap().remove(missing);
        assert!(serde_json::from_value::<Response>(invalid).is_err());
    }
    let decoded: Response = serde_json::from_value(wire.clone()).unwrap();
    assert!(
        decoded
            .validate_for(&req, &BootId("another-boot".into()))
            .is_err()
    );
    let mut wrong_req = req.clone();
    wrong_req.request_id = "another-request".into();
    assert!(
        decoded
            .validate_for(&wrong_req, &BootId("synthetic-boot".into()))
            .is_err()
    );
    wrong_req = req;
    wrong_req.call = Call::Status;
    assert!(
        decoded
            .validate_for(&wrong_req, &BootId("synthetic-boot".into()))
            .is_err()
    );
    let mut invalid = wire;
    invalid["outcome"]["result"]["value"]["credential"] = json!("unexpected");
    assert!(serde_json::from_value::<Response>(invalid).is_err());
}
#[test]
fn errors_and_unsupported_request_versions_are_explicit_and_correlated() {
    for (method, params, _) in fixtures() {
        let mut req = request(method, params);
        let error = |code| ResponseOutcome::Error {
            error: ApiError {
                code,
                phase: "preflight".into(),
                recovery: "configure deliberately".into(),
            },
        };
        let response = Response::for_request(
            &req,
            BootId("synthetic-boot".into()),
            error(ErrorCode::BackendUnavailable),
        )
        .unwrap();
        let value = serde_json::to_value(response).unwrap();
        assert_eq!(value["outcome"]["status"], "error");
        assert!(value["outcome"].get("result").is_none());
        assert!(!value.to_string().contains("SYNTHETIC_PRIVATE"));
        let roundtrip: Response = serde_json::from_value(value).unwrap();
        assert!(
            roundtrip
                .validate_for(&req, &BootId("synthetic-boot".into()))
                .is_ok()
        );
        req.version = 2;
        assert!(
            Response::for_request(
                &req,
                BootId("synthetic-boot".into()),
                error(ErrorCode::BackendUnavailable)
            )
            .is_err()
        );
        assert!(
            Response::for_request(
                &req,
                BootId("synthetic-boot".into()),
                error(ErrorCode::ProtocolVersionUnsupported)
            )
            .is_ok()
        );
    }
}
#[test]
fn uncertainty_cleanup_media_partial_and_capability_evidence_survive_serialization() {
    let data = fixtures();
    let (_, _, action) = data.iter().find(|(m, _, _)| *m == "input.type").unwrap();
    let action: ActionResult = serde_json::from_value(action.clone()).unwrap();
    assert_eq!(action.receipt.input_status, InputStatus::Unknown);
    assert_eq!(
        action.receipt.observation_status,
        ObservationStatus::Timeout
    );
    assert_eq!(action.receipt.cleanup, Cleanup::Pending);
    assert!(action.observation.is_none());
    let caps: DeviceCapabilities = serde_json::from_value(capabilities()).unwrap();
    assert_eq!(caps.aoa_touch.evidence, Evidence::Unavailable);
    assert_eq!(caps.screenshot.evidence, Evidence::Detected);
    assert_eq!(caps.ocr.evidence, Evidence::Verified);
    let (_, _, media) = data.iter().find(|(m, _, _)| *m == "media.push").unwrap();
    let decoded: MediaPushResult = serde_json::from_value(media.clone()).unwrap();
    assert!(matches!(decoded.media.copied, Stage::Known(true)));
    assert!(matches!(decoded.media.indexed, Stage::Error(_)));
    assert!(matches!(decoded.media.picker_verified, Stage::Unknown));
}

#[test]
fn active_action_status_is_distinct_from_a_final_receipt() {
    for phase in ["queued", "executing"] {
        let value = json!({"phase":phase,"actionId":"synthetic-action","seq":1});
        let status: ActionStatusResult = serde_json::from_value(value.clone()).unwrap();
        assert_eq!(serde_json::to_value(status).unwrap(), value);
        let invalid = json!({"phase":phase,"actionId":"synthetic-action","seq":1,"action":{"receipt":receipt(),"observation":null}});
        assert!(serde_json::from_value::<ActionStatusResult>(invalid).is_err());
    }
    let version_schema = serde_json::to_value(schemars::schema_for!(ProtocolVersion)).unwrap();
    assert_eq!(version_schema["const"], 1);
    assert_eq!(version_schema["type"], "integer");
}
