//! Explicit SOFTWARE FAKE Python fixtures; never a real ADB executable.
use std::{
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
};
pub fn source(path: &Path) -> PathBuf {
    path.with_extension("script")
}
pub fn python(path: &Path, script: &str) {
    let source = source(path);
    std::fs::write(&source, script).unwrap();
    std::fs::set_permissions(&source, std::fs::Permissions::from_mode(0o600)).unwrap();
    // The interpreter executes only this immutable checked-in file. Other
    // children may inherit writable program-data FDs without making it ETXTBSY.
    std::os::unix::fs::symlink(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/fake-adb-runner.py"),
        path,
    )
    .unwrap();
}
