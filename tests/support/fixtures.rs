//! Shared filesystem setup for synthetic tests; never relies on test ordering.
use std::path::{Path, PathBuf};

pub fn build_root() -> PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("build");
    std::fs::create_dir_all(&root).expect("create test build parent");
    root
}

#[allow(dead_code)] // Some integration targets need only a persistent build path.
pub fn private_tempdir() -> tempfile::TempDir {
    let mut builder = tempfile::Builder::new();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        builder.permissions(std::fs::Permissions::from_mode(0o700));
    }
    builder
        .tempdir_in(build_root())
        .expect("create private fixture")
}
