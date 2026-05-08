/// Detect the filesystem type present on an SDMMC/eMMC card by reading raw
/// sectors with `sdmmc_read_sectors` and inspecting magic bytes.
///
/// Detection strategy (in order):
/// 1. **EXT4** – reads sector 2 (byte offset 1024) and checks for the EXT2/3/4
///    superblock magic `0xEF53` at superblock offset `0x38`.
/// 2. **FAT** – reads sector 0 and checks for the MBR/boot-sector signature
///    `0x55 0xAA` at bytes 510–511.
use crate::{FsType, PartitionInfo};
use esp_idf_sys::{sdmmc_card_t, sdmmc_read_sectors, ESP_OK};

/// Sector index where the EXT2/3/4 superblock starts (byte offset 1024 for
/// 512-byte sectors).
const EXT4_SUPERBLOCK_SECTOR: usize = 2;

/// Offset of the superblock magic within a 512-byte sector read starting at
/// `EXT4_SUPERBLOCK_SECTOR`.  The superblock magic sits at absolute byte 1080
/// (0x38 within the superblock which starts at byte 1024).
const EXT4_MAGIC_OFFSET: usize = 0x38;

/// EXT2/3/4 superblock magic: little-endian 0xEF53.
const EXT4_MAGIC: [u8; 2] = [0x53, 0xEF];

/// FAT boot-sector / MBR signature at bytes 510–511.
const FAT_SIG_OFFSET: usize = 510;
const FAT_SIG: [u8; 2] = [0x55, 0xAA];

/// Probe the card and return the detected [`FsType`].
///
/// # Safety
/// `card` must be a valid, mounted `sdmmc_card_t` pointer returned by an
/// `sdmmc_card_init` or `esp_vfs_fat_sdmmc_mount` call.
pub unsafe fn detect(card: *mut sdmmc_card_t) -> FsType {
    let mut buf = [0u8; 512];

    // --- EXT4 probe ---
    let rc = sdmmc_read_sectors(
        card,
        buf.as_mut_ptr() as *mut core::ffi::c_void,
        EXT4_SUPERBLOCK_SECTOR,
        1,
    );
    if rc == ESP_OK as i32
        && buf[EXT4_MAGIC_OFFSET] == EXT4_MAGIC[0]
        && buf[EXT4_MAGIC_OFFSET + 1] == EXT4_MAGIC[1]
    {
        return FsType::Ext4;
    }

    // --- FAT probe ---
    let rc = sdmmc_read_sectors(
        card,
        buf.as_mut_ptr() as *mut core::ffi::c_void,
        0,
        1,
    );
    if rc == ESP_OK as i32
        && buf[FAT_SIG_OFFSET] == FAT_SIG[0]
        && buf[FAT_SIG_OFFSET + 1] == FAT_SIG[1]
    {
        return FsType::Fat;
    }

    FsType::Unknown
}

/// Read the MBR partition table from sector 0 and return up to 4 partition
/// entries.  Returns an empty Vec if the disk has no valid MBR signature or
/// if the sector read fails.
///
/// # Safety
/// `card` must be a valid, initialised `sdmmc_card_t` pointer.
pub unsafe fn scan_mbr(card: *mut sdmmc_card_t) -> Vec<PartitionInfo> {
    let mut buf = [0u8; 512];
    let rc = sdmmc_read_sectors(
        card,
        buf.as_mut_ptr() as *mut core::ffi::c_void,
        0,
        1,
    );
    if rc != ESP_OK as i32 {
        return Vec::new();
    }
    // Validate MBR signature.
    if buf[510] != 0x55 || buf[511] != 0xAA {
        return Vec::new();
    }
    let mut parts = Vec::new();
    for i in 0..4usize {
        let o = 446 + i * 16;
        let part_type = buf[o + 4];
        if part_type == 0 {
            continue;
        }
        let lba_start = u32::from_le_bytes([buf[o+8], buf[o+9], buf[o+10], buf[o+11]]);
        let sector_count = u32::from_le_bytes([buf[o+12], buf[o+13], buf[o+14], buf[o+15]]);
        if lba_start == 0 {
            continue;
        }
        parts.push(PartitionInfo {
            part_type,
            lba_start,
            sector_count,
            bootable: buf[o] == 0x80,
        });
    }
    parts
}
