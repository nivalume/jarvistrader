//! The error code of a fallible kernel operation.

/// Why a kernel operation did not complete. The kernel never panics on input; an operation that
/// can fail returns `Result<T, Status>` ([`Result`]).
///
/// The discriminants and names are those of `jarvis::core::Status` in the C++ tree, minus its
/// `Ok = 0`: success is `Ok(value)` here. Names appear in log messages and tool output, so they
/// are stable.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u8)]
pub enum Status {
    InvalidArgument = 1,
    OutOfRange = 2,
    Overflow = 3,
    PrecisionLoss = 4,
    ParseError = 5,
    CapacityExceeded = 6,
    NotFound = 7,
    AlreadyExists = 8,
    InvalidState = 9,
    InvalidTransition = 10,
    DuplicateFill = 11,
    UnsupportedMessage = 12,
    ChecksumMismatch = 13,
    Truncated = 14,
    EndOfStream = 15,
    IoError = 16,
    /// Nothing yet: a live source that has no input at the moment, but will have.
    WouldBlock = 17,
}

/// The result of a fallible kernel operation.
pub type Result<T> = core::result::Result<T, Status>;

impl Status {
    /// The name, as the C++ tree's `to_string(Status)` writes it.
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Status::InvalidArgument => "InvalidArgument",
            Status::OutOfRange => "OutOfRange",
            Status::Overflow => "Overflow",
            Status::PrecisionLoss => "PrecisionLoss",
            Status::ParseError => "ParseError",
            Status::CapacityExceeded => "CapacityExceeded",
            Status::NotFound => "NotFound",
            Status::AlreadyExists => "AlreadyExists",
            Status::InvalidState => "InvalidState",
            Status::InvalidTransition => "InvalidTransition",
            Status::DuplicateFill => "DuplicateFill",
            Status::UnsupportedMessage => "UnsupportedMessage",
            Status::ChecksumMismatch => "ChecksumMismatch",
            Status::Truncated => "Truncated",
            Status::EndOfStream => "EndOfStream",
            Status::IoError => "IoError",
            Status::WouldBlock => "WouldBlock",
        }
    }

    /// The numeric code shared with the C++ tree (0 is success there).
    #[must_use]
    pub const fn code(self) -> u8 {
        self as u8
    }
}

impl core::fmt::Display for Status {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.as_str())
    }
}
