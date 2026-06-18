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

/// GPT primary header signature ("EFI PART").
const GPT_SIGNATURE: &[u8; 8] = b"EFI PART";

/// Type GUID for Linux filesystem partitions stored in GPT mixed-endian format.
/// UUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
const GPT_LINUX_DATA_GUID: [u8; 16] = [
    0xAF, 0x3D, 0xC6, 0x0F,  // 0FC63DAF (LE)
    0x83, 0x84,              // 8483 (LE)
    0x72, 0x47,              // 4772 (LE)
    0x8E, 0x79,              // 8E79 (BE)
    0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4, // 3D69D8477DE4 (BE)
];

/// Scan the GPT partition table and return all recognised partitions.
/// Linux data partitions (type GUID 0FC63DAF-…) are mapped to `part_type = 0x83`.
/// Other non-empty entries are included with `part_type = 0xFF`.
///
/// Returns an empty Vec when the disk has no valid GPT header or if sector
/// reads fail.
///
/// # Safety
/// `card` must be a valid, initialised `sdmmc_card_t` pointer.
pub unsafe fn scan_gpt(card: *mut sdmmc_card_t) -> Vec<PartitionInfo> {
    let mut buf = [0u8; 512];

    // Sector 1 = GPT primary header.
    let rc = sdmmc_read_sectors(
        card,
        buf.as_mut_ptr() as *mut core::ffi::c_void,
        1,
        1,
    );
    if rc != ESP_OK as i32 {
        return Vec::new();
    }

    if &buf[0..8] != GPT_SIGNATURE {
        return Vec::new();
    }

    // Partition entry start LBA (header offset 72, 8 bytes LE).
    let entry_lba = u64::from_le_bytes([
        buf[72], buf[73], buf[74], buf[75],
        buf[76], buf[77], buf[78], buf[79],
    ]);
    // Number of partition entries (header offset 80, 4 bytes LE).
    let num_entries = u32::from_le_bytes([buf[80], buf[81], buf[82], buf[83]]);
    // Size of each partition entry in bytes (header offset 84, 4 bytes LE).
    let entry_size = u32::from_le_bytes([buf[84], buf[85], buf[86], buf[87]]);

    if entry_size != 128 || num_entries == 0 || entry_lba == 0 {
        return Vec::new();
    }

    let max_entries = num_entries.min(128) as usize;
    let entries_per_sector = 512 / entry_size as usize; // 4 for 128-byte entries
    let sectors_needed = (max_entries + entries_per_sector - 1) / entries_per_sector;

    let mut parts = Vec::new();
    for s in 0..sectors_needed {
        let lba = entry_lba + s as u64;
        let rc = sdmmc_read_sectors(
            card,
            buf.as_mut_ptr() as *mut core::ffi::c_void,
            lba as usize,
            1,
        );
        if rc != ESP_OK as i32 {
            break;
        }

        for e in 0..entries_per_sector {
            if s * entries_per_sector + e >= max_entries {
                break;
            }
            let off = e * entry_size as usize;

            // Skip unused entries (all-zero type GUID).
            if buf[off..off + 16].iter().all(|&b| b == 0) {
                continue;
            }

            let first_lba = u64::from_le_bytes([
                buf[off+32], buf[off+33], buf[off+34], buf[off+35],
                buf[off+36], buf[off+37], buf[off+38], buf[off+39],
            ]);
            let last_lba = u64::from_le_bytes([
                buf[off+40], buf[off+41], buf[off+42], buf[off+43],
                buf[off+44], buf[off+45], buf[off+46], buf[off+47],
            ]);

            if first_lba == 0 || last_lba < first_lba {
                continue;
            }

            let part_type: u8 = if buf[off..off+16] == GPT_LINUX_DATA_GUID {
                0x83 // Linux ext2/3/4
            } else {
                0xFF // Other GPT partition
            };

            parts.push(PartitionInfo {
                part_type,
                lba_start: first_lba.min(u32::MAX as u64) as u32,
                sector_count: (last_lba - first_lba + 1).min(u32::MAX as u64) as u32,
                bootable: false,
            });
        }
    }
    parts
}

/// Scan for partitions trying MBR first, then GPT.
///
/// When the MBR contains only a GPT protective entry (type 0xEE) the
/// function switches to GPT scanning automatically.
///
/// # Safety
/// `card` must be a valid, initialised `sdmmc_card_t` pointer.
pub unsafe fn scan_partitions(card: *mut sdmmc_card_t) -> Vec<PartitionInfo> {
    let mbr = scan_mbr(card);
    // A disk with a GPT has a protective MBR whose single entry is type 0xEE.
    let all_gpt_protective = !mbr.is_empty() && mbr.iter().all(|p| p.part_type == 0xEE);
    if mbr.is_empty() || all_gpt_protective {
        let gpt = scan_gpt(card);
        if !gpt.is_empty() {
            return gpt;
        }
    }
    mbr
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
