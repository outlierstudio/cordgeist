//! Explicit SOFTWARE FAKE ASCII execution. No USB or physical-layout evidence.
use cordgeist::{api::ErrorCode, ascii::Text, backend::Cancel, session::*, session_input::*};
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};
struct Fake {
    events: Arc<Mutex<Vec<(u16, bool)>>>,
    fault_at: Option<usize>,
    cancel: Option<Cancel>,
    cleanup_fail: bool,
}
impl SessionDriver for Fake {
    type Handle = ();
    fn open(
        &mut self,
        _: Resource,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn close(&mut self, r: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        CloseOutcome::Consumed(if self.cleanup_fail && r == Resource::KeyboardNode {
            Err(Failure::code(ErrorCode::BackendUnavailable))
        } else {
            Ok(())
        })
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        _: &Handles<()>,
        b: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        assert!((1..=500).contains(&b.control_ms));
        let Transition::Key { usage, down } = t else {
            panic!("expected key transition")
        };
        let count = {
            let mut e = self.events.lock().unwrap();
            e.push((*usage, *down));
            e.len()
        };
        if self.fault_at == Some(count) {
            if let Some(cancel) = &self.cancel {
                cancel.cancel();
            }
            return ReportResult {
                delivery: Delivery::Unknown,
                error: Some(Failure::code(ErrorCode::ProcessTimeout)),
            };
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
type Events = Arc<Mutex<Vec<(u16, bool)>>>;
async fn owner(fault_at: Option<usize>, cancel: Option<Cancel>) -> (Session, Events) {
    let events = Arc::new(Mutex::new(vec![]));
    let e = events.clone();
    let s = Session::start(Duration::from_secs(1), move || {
        Ok(Fake {
            events: e,
            fault_at,
            cancel,
            cleanup_fail: fault_at.is_some(),
        })
    })
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    (s, events)
}
fn text(payload: &str) -> Transition {
    Transition::Text(Text::new("us-ansi", payload))
}
#[tokio::test]
async fn uppercase_symbols_tab_lf_and_modifier_release_follow_policy() {
    let (s, e) = owner(None, None).await;
    assert_eq!(
        s.submit(text("A!\t\n"), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::Sent
    );
    assert_eq!(
        *e.lock().unwrap(),
        vec![
            (0xe1, true),
            (4, true),
            (4, false),
            (0xe1, false),
            (0xe1, true),
            (0x1e, true),
            (0x1e, false),
            (0xe1, false),
            (0x2b, true),
            (0x2b, false),
            (0x28, true),
            (0x28, false)
        ]
    );
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn trailing_invalid_payload_layout_and_limits_reject_without_prefix() {
    let (s, e) = owner(None, None).await;
    for payload in ["aé", "a\r", "a\0", "a\u{7f}", ""] {
        assert_eq!(
            s.submit(text(payload), Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        assert!(e.lock().unwrap().is_empty());
    }
    assert_eq!(
        s.submit(
            Transition::Text(Text::new("unknown", "a")),
            Duration::from_secs(1),
            &Cancel::default()
        )
        .await
        .delivery,
        Delivery::NotSent
    );
    assert_eq!(
        s.submit(
            text(&"a".repeat(4097)),
            Duration::from_secs(1),
            &Cancel::default()
        )
        .await
        .delivery,
        Delivery::NotSent
    );
    assert_eq!(
        s.submit(text("a"), Duration::from_secs(121), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert!(e.lock().unwrap().is_empty());
    assert_eq!(
        s.submit(text("a"), Duration::from_secs(120), &Cancel::default())
            .await
            .delivery,
        Delivery::Sent
    );
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn inherited_modifier_is_not_used_or_released_by_rejected_text() {
    let (s, e) = owner(None, None).await;
    let key = |down| Transition::Key { usage: 0xe1, down };
    s.submit(key(true), Duration::from_secs(1), &Cancel::default())
        .await;
    assert_eq!(
        s.submit(text("a"), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(e.lock().unwrap().len(), 1);
    assert_eq!(
        s.submit(key(false), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::Sent
    );
    assert_eq!(
        s.submit(text("a"), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::Sent
    );
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn uncertain_or_cancelled_shifted_text_fences_and_preserves_cleanup_failure() {
    for cancelling in [false, true] {
        let cancel = Cancel::default();
        let (s, e) = owner(
            Some(2),
            if cancelling {
                Some(cancel.clone())
            } else {
                None
            },
        )
        .await;
        let result = s.submit(text("AB"), Duration::from_secs(1), &cancel).await;
        assert!(matches!(
            result.delivery,
            Delivery::Partial | Delivery::Unknown
        ));
        assert_eq!(
            s.submit(text("AB"), Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
        assert!(closed.cleanup_error.is_some());
        assert_eq!(e.lock().unwrap().len(), 2);
    }
}

#[tokio::test]
async fn shifted_symbol_row_uses_explicit_us_ansi_usages() {
    let (s, e) = owner(None, None).await;
    let result = s
        .submit(
            text("!@#$%^&*()_+{}|:\"~<>?"),
            Duration::from_secs(1),
            &Cancel::default(),
        )
        .await;
    assert_eq!(result.delivery, Delivery::Sent);
    let usages = [
        0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2f, 0x30, 0x31,
        0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    ];
    {
        let events = e.lock().unwrap();
        assert_eq!(events.len(), usages.len() * 4);
        for (chord, usage) in events.chunks_exact(4).zip(usages) {
            assert_eq!(
                chord,
                [(0xe1, true), (usage, true), (usage, false), (0xe1, false)]
            );
        }
    }
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn maximum_ascii_payload_uses_existing_text_limit_and_neutral_chords() {
    let (s, e) = owner(None, None).await;
    assert_eq!(
        s.submit(
            text(&"a".repeat(4096)),
            Duration::from_secs(120),
            &Cancel::default()
        )
        .await
        .delivery,
        Delivery::Sent
    );
    {
        let events = e.lock().unwrap();
        assert_eq!(events.len(), 8192);
        assert!(
            events
                .chunks_exact(2)
                .all(|chord| chord == [(4, true), (4, false)])
        );
    }
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
