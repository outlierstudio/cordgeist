//! Ready session that owns no USB handles and refuses every input report.
use crate::{
    api::ErrorCode,
    backend::Cancel,
    session::{CallBudget, CloseOutcome, Failure, Handles, Resource, SessionDriver},
    session_input::{Delivery, ReportResult, Transition},
};

pub struct ObserveSession;

impl SessionDriver for ObserveSession {
    type Handle = ();

    fn validate_report(&mut self, _: &Transition, _: &Cancel, _: &Cancel) -> Result<(), Failure> {
        Err(Failure::code(ErrorCode::MappingUnverified))
    }

    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Err(Failure::code(ErrorCode::MappingUnverified))
    }

    fn submit_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        ReportResult {
            delivery: Delivery::NotSent,
            error: Some(Failure::code(ErrorCode::MappingUnverified)),
        }
    }

    fn open(
        &mut self,
        _: Resource,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }

    fn close(&mut self, _: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        CloseOutcome::Consumed(Ok(()))
    }
}
