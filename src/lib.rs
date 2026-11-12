pub mod adb;
pub mod api;
pub mod backend;
pub mod core;
pub mod diagnostics;
pub mod discovery;
#[cfg(target_os = "linux")]
pub mod discovery_service;
pub mod geometry;
pub mod identity;
#[cfg(test)]
mod invariants;
pub mod metadata;
#[cfg(feature = "native-aoa")]
pub mod native;
pub mod observation;
#[cfg(target_os = "linux")]
pub mod pairing_store;
#[cfg(any(test, feature = "fake"))]
pub mod testkit;

pub mod session;

pub mod session_input;

pub mod gesture;

pub mod ascii;

#[cfg(target_os = "linux")]
pub mod ipc;

pub mod actor;

mod lease;
pub mod lease_devices;
mod reconnect;

pub mod frames;

pub mod cli_app;
pub mod daemon;
pub mod export;
pub mod lease_file;
#[cfg(target_os = "linux")]
pub mod mcp;
pub mod media;
pub mod service;
#[cfg(feature = "fake")]
pub mod software_fixture;

pub mod calibration;

pub mod test_area;

mod mcp_authority;

pub mod android_ui;

#[cfg(all(target_os = "linux", feature = "native-aoa"))]
mod android_control;

pub mod doctor;

#[cfg(test)]
#[path = "../tests/support/fixtures.rs"]
mod fixtures;
