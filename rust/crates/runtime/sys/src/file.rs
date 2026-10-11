//! Files written to survive a crash (docs/architecture.md section 16): written through the
//! operating system's own handle so that they can be synced, and replaced atomically.
//!
//! - Linux and macOS: `open`, `fdatasync` (`fsync` on macOS, where std's `sync_data` maps to it),
//!   `fsync` of the directory, `rename`;
//! - Windows: `CreateFileW`, `FlushFileBuffers`, `MoveFileExW(REPLACE_EXISTING)`; a directory
//!   needs no sync (NTFS journals its entries).

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::Path;

use kernel_core::{Result, Status};

/// How a file is opened for writing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Fails with `AlreadyExists` if the file exists.
    CreateNew,
    /// Creates it, or empties it.
    Truncate,
}

/// A file open for appending durable data.
#[derive(Debug)]
pub struct DurableFile {
    file: File,
}

fn status_of(e: &io::Error) -> Status {
    match e.kind() {
        io::ErrorKind::AlreadyExists => Status::AlreadyExists,
        io::ErrorKind::NotFound => Status::NotFound,
        _ => Status::IoError,
    }
}

impl DurableFile {
    /// Opens `path` for writing. `IoError`, or `AlreadyExists` for `CreateNew`, on failure.
    pub fn open(path: &Path, mode: Mode) -> Result<Self> {
        let mut options = OpenOptions::new();
        options.write(true);
        match mode {
            Mode::CreateNew => options.create_new(true),
            Mode::Truncate => options.create(true).truncate(true),
        };
        let file = options.open(path).map_err(|e| status_of(&e))?;
        Ok(Self { file })
    }

    pub fn write_all(&mut self, bytes: &[u8]) -> Result<()> {
        self.file.write_all(bytes).map_err(|_| Status::IoError)
    }

    /// The data and what is needed to read it back (`fdatasync`).
    pub fn sync_data(&mut self) -> Result<()> {
        self.file.sync_data().map_err(|_| Status::IoError)
    }

    /// The data and the rest of the file's metadata (`fsync`).
    pub fn sync(&mut self) -> Result<()> {
        self.file.sync_all().map_err(|_| Status::IoError)
    }
}

/// Makes the entries created or renamed in `directory` survive a crash. A no-op on Windows.
pub fn sync_directory(directory: &Path) -> Result<()> {
    if cfg!(windows) {
        return Ok(());
    }
    let dir = if directory.as_os_str().is_empty() { Path::new(".") } else { directory };
    File::open(dir).and_then(|d| d.sync_all()).map_err(|_| Status::IoError)
}

/// Replaces `to` with `from` in one step: a reader sees the old file or the new one, never a mix.
pub fn replace_file(from: &Path, to: &Path) -> Result<()> {
    fs::rename(from, to).map_err(|e| status_of(&e))
}

/// Writes `text` to `path` through a temporary file in the same directory, synced and renamed into
/// place, so the file is whole or absent.
pub fn write_text_atomic(path: &Path, text: &[u8]) -> Result<()> {
    let name = path.file_name().ok_or(Status::InvalidArgument)?;
    let mut tmp_name = name.to_os_string();
    tmp_name.push(".tmp");
    let tmp = path.with_file_name(tmp_name);
    {
        let mut file = DurableFile::open(&tmp, Mode::Truncate)?;
        file.write_all(text)?;
        file.sync_data()?;
    }
    replace_file(&tmp, path)?;
    sync_directory(path.parent().unwrap_or(Path::new(".")))
}

/// A whole file; `NotFound` or `IoError`.
pub fn read_bytes(path: &Path) -> Result<Vec<u8>> {
    fs::read(path).map_err(|e| status_of(&e))
}

/// A whole UTF-8 file.
pub fn read_text(path: &Path) -> Result<String> {
    String::from_utf8(read_bytes(path)?).map_err(|_| Status::ParseError)
}
