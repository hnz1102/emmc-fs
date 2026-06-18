//! `emmc-fs` — FAT and EXT4 filesystem abstraction for ESP32 eMMC/SD cards.
//!
//! # Features
//! | Feature | Description |
//! |---------|-------------|
//! | `fat`   | FAT12/16/32 support via `esp_vfs_fat` (enabled by default) |
//! | `ext4`  | EXT2/3/4 support via the `lwext4_blockdev` ESP-IDF component |
//!
//! # Quick start
//! ```no_run
//! use emmc_fs::{FsType, detect};
//! use emmc_fs::fat::FatMount;
//! # #[cfg(feature = "ext4")]
//! use emmc_fs::ext4::Ext4Mount;
//! ```
//!
//! # EXT4 prerequisites
//! Clone the lwext4 source next to the component wrapper before building:
//! ```sh
//! cd emmc-fs/components/lwext4_blockdev
//! git clone https://github.com/gkostka/lwext4 lwext4
//! ```

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

mod error;
pub use error::{EmmcError, FsType};

pub mod detect;

#[cfg(feature = "fat")]
pub mod fat;

#[cfg(feature = "ext4")]
pub mod ext4;

/// Convenience re-export: detect the filesystem type on a raw card pointer.
///
/// # Safety
/// `card` must be a valid `sdmmc_card_t` pointer that has been fully
/// initialised (e.g. returned from a successful `sdmmc_card_init` call).
pub use detect::detect;

/// Re-export MBR scanner.
pub use detect::scan_mbr;

/// Re-export combined MBR/GPT partition scanner.
pub use detect::scan_partitions;

/// Information about a single MBR partition entry.
#[derive(Clone, Debug)]
pub struct PartitionInfo {
    /// Partition type code (0x0B/0x0C = FAT32, 0x83 = Linux ext4, …).
    pub part_type: u8,
    /// First LBA of the partition on the physical device.
    pub lba_start: u32,
    /// Number of 512-byte sectors in the partition.
    pub sector_count: u32,
    /// True if the bootable flag (0x80) is set.
    pub bootable: bool,
}

impl PartitionInfo {
    /// Returns true if this is a FAT12/16/32 partition.
    pub fn is_fat(&self) -> bool {
        matches!(self.part_type,
            0x01 | 0x04 | 0x06 | 0x0B | 0x0C | 0x0E | 0x0F |
            0x11 | 0x14 | 0x16 | 0x1B | 0x1C | 0x1E |
            0xEF) // EFI System Partition (FAT32 contents)
    }

    /// Returns true if this is a Linux filesystem (ext2/3/4) partition.
    pub fn is_ext(&self) -> bool {
        self.part_type == 0x83
    }

    /// Human-readable type name.
    pub fn type_name(&self) -> &'static str {
        match self.part_type {
            0x01 => "FAT12",
            0x04 | 0x06 => "FAT16",
            0x0B | 0x0C => "FAT32",
            0x0E => "FAT16B",
            0x0F => "FAT32 Extended",
            0x82 => "Linux swap",
            0x83 => "Linux ext4",
            0xEE => "GPT protective",
            0xEF => "EFI System",
            _ => "Unknown",
        }
    }

    /// Size of the partition in bytes.
    pub fn size_bytes(&self) -> u64 {
        self.sector_count as u64 * 512
    }
}
