//! What identifies this build in log headers and snapshot files. A snapshot is restored only by
//! the build that wrote it: the state layout is the build's.

/// The crate version.
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The git commit the build was made from, when the build set `JARVIS_GIT_COMMIT`.
#[must_use]
pub fn git_commit() -> &'static str {
    option_env!("JARVIS_GIT_COMMIT").unwrap_or("unknown")
}

/// `jarvis-rs <version> <commit>`: the `build` field of a log header.
#[must_use]
pub fn build_text() -> String {
    format!("jarvis-rs {VERSION} {}", git_commit())
}

/// `<os>-<arch>`: the `platform` field of a log header.
#[must_use]
pub fn platform_text() -> String {
    format!("{}-{}", std::env::consts::OS, std::env::consts::ARCH)
}
