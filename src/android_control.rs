//! Explicitly authorized Android app/button controls, on the existing owner thread.
//! Touch/swipe/ASCII never use this path. Every outcome identifies ADB transport.
use crate::{
    api::*,
    backend::Cancel,
    session::Failure,
    session_input::{Delivery, ReportResult},
};
use std::time::Instant;
pub(crate) fn submit(
    target: &crate::adb::Target,
    command: &AndroidCommand,
    deadline: Instant,
    cancel: &Cancel,
    external: &Cancel,
) -> ReportResult {
    let denied = |code| ReportResult {
        delivery: Delivery::NotSent,
        error: Some(Failure::at(code, "adb-app-or-button")),
    };
    if let Err(code) = command
        .validate()
        .and_then(|()| command.shell_command().map(|_| ()))
    {
        return denied(code);
    }
    if cancel.cancelled() || external.cancelled() {
        return denied(ErrorCode::ProcessCancelled);
    }
    if Instant::now() >= deadline {
        return denied(ErrorCode::ProcessTimeout);
    }
    let Some(path) = crate::adb::configured_path() else {
        return denied(ErrorCode::BackendUnavailable);
    };
    let adb = match crate::adb::Adb::new(
        path,
        std::env::var("CORDGEIST_ADB_PORT")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(5037),
    ) {
        Ok(adb) => adb,
        Err(code) => return denied(code),
    };
    let runtime = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(rt) => rt,
        Err(_) => return denied(ErrorCode::BackendUnavailable),
    };
    let result=runtime.block_on(async {
        let token=tokio_util::sync::CancellationToken::new();
        let work=adb.android_command(target,command,deadline,token.clone());tokio::pin!(work);
        loop {tokio::select! {result=&mut work=>break result,_=tokio::time::sleep(std::time::Duration::from_millis(2))=>{
            if cancel.cancelled()||external.cancelled()||Instant::now()>=deadline {token.cancel();}
        }}}
    });
    match result {
        Ok(done) if done.success => ReportResult {
            delivery: Delivery::Sent,
            error: None,
        },
        Ok(_) => ReportResult {
            delivery: Delivery::Unknown,
            error: Some(Failure::at(
                ErrorCode::ProcessFailed,
                "adb-app-or-button-exit",
            )),
        },
        Err(code) => ReportResult {
            delivery: Delivery::Unknown,
            error: Some(Failure::at(code, "adb-app-or-button")),
        },
    }
}
