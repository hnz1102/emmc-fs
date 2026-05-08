/// FAT or EXT4 filesystem type detected on the device.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FsType {
    /// FAT12 / FAT16 / FAT32 / exFAT — identified by 0x55AA boot signature.
    Fat,
    /// EXT2 / EXT3 / EXT4 — identified by 0xEF53 superblock magic.
    Ext4,
    /// Could not identify a known filesystem.
    Unknown,
}

/// Errors produced by emmc-fs operations.
#[derive(Debug)]
pub enum EmmcError {
    /// An ESP-IDF function returned a non-OK error code.
    EspError(i32),
    /// Filesystem type was not recognised.
    UnknownFs,
    /// A null-terminated C string could not be created from the given bytes.
    BadMountPoint,
    /// The requested operation is not supported for this filesystem type.
    Unsupported,
}

impl core::fmt::Display for EmmcError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            EmmcError::EspError(code) => write!(f, "ESP-IDF error 0x{:x}", code),
            EmmcError::UnknownFs => write!(f, "unknown filesystem"),
            EmmcError::BadMountPoint => write!(f, "invalid mount-point string"),
            EmmcError::Unsupported => write!(f, "operation not supported"),
        }
    }
}

impl std::error::Error for EmmcError {}

impl From<i32> for EmmcError {
    fn from(code: i32) -> Self {
        EmmcError::EspError(code)
    }
}
