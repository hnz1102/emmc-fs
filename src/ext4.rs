/// EXT4 filesystem support via the `lwext4_blockdev` ESP-IDF component.
///
/// # Prerequisites
/// 1. Clone lwext4 into the component directory:
///    ```sh
///    cd emmc-fs/components/lwext4_blockdev
///    git clone https://github.com/gkostka/lwext4 lwext4
///    ```
/// 2. Build with `--features ext4`.
///
/// Mounting registers the EXT4 VFS at the chosen mount point so that standard
/// `std::fs` operations work on EXT4 files the same way as on FAT.
use crate::EmmcError;
use esp_idf_sys::{sdmmc_card_t, sdmmc_host_t, sdmmc_slot_config_t, ESP_OK};
use std::ffi::{c_void, CString};

// ---------------------------------------------------------------------------
// FFI declarations for lwext4_blockdev C component
// ---------------------------------------------------------------------------

extern "C" {
    /// Initialise the lwext4 SDMMC block device with the given card.
    /// Must be called after the card has been initialised by `sdmmc_card_init`.
    fn lwext4_blockdev_init(card: *mut sdmmc_card_t) -> i32;

    /// Release the lwext4 block device registration.
    fn lwext4_blockdev_deinit() -> i32;

    /// Restrict block-device access to a specific MBR partition window.
    /// Must be called after `lwext4_blockdev_init` and before `lwext4_mount`.
    fn lwext4_blockdev_set_partition(lba_start: u64, sector_count: u64) -> i32;

    /// Format the registered block device as EXT4.  Must be called after
    /// `lwext4_blockdev_init` but before `lwext4_mount`.
    /// `label` may be null.
    fn lwext4_format(label: *const core::ffi::c_char) -> i32;

    /// Mount the EXT4 filesystem at `mount_point` (null-terminated C string).
    fn lwext4_mount(mount_point: *const u8) -> i32;

    /// Unmount the EXT4 filesystem previously mounted at `mount_point`.
    fn lwext4_umount(mount_point: *const u8) -> i32;

    /// Initialise the card via SDMMC host + slot config and return the card pointer.
    fn lwext4_sdmmc_card_init(
        host: *const sdmmc_host_t,
        slot_config: *const c_void,
        card_out: *mut *mut sdmmc_card_t,
    ) -> i32;

    /// Deinitialise the SDMMC host and free the card struct.
    fn lwext4_sdmmc_card_deinit(host: *const sdmmc_host_t, card: *mut sdmmc_card_t);
}

// ---------------------------------------------------------------------------
// Safe Rust wrapper
// ---------------------------------------------------------------------------

/// A mounted EXT4 filesystem.  Unmounts and releases the block device on drop.
pub struct Ext4Mount {
    /// Raw card pointer (allocated by lwext4_sdmmc_card_init).
    pub card: *mut sdmmc_card_t,
    /// Snapshot of the host descriptor used during init (for deinit on drop).
    host_snapshot: sdmmc_host_t,
    mount_point: CString,
}

// The card pointer is only accessed from the owning thread.
unsafe impl Send for Ext4Mount {}

impl Ext4Mount {
    /// Mount the eMMC/SD card as EXT4 at `mount_point`.
    ///
    /// # Arguments
    /// * `host` – SDMMC host descriptor (already configured).
    /// * `slot_config` – Slot pin / width configuration.
    /// * `mount_point` – Mount path, e.g. `"/ext4"`.  Must not contain a
    ///   null byte.
    ///
    /// # Safety
    /// All pointer arguments must remain valid for the lifetime of this mount.
    pub unsafe fn mount(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &str,
    ) -> Result<Self, EmmcError> {
        let mp = CString::new(mount_point).map_err(|_| EmmcError::BadMountPoint)?;

        // Initialise the card via SDMMC
        let mut card: *mut sdmmc_card_t = core::ptr::null_mut();
        let rc = lwext4_sdmmc_card_init(
            host as *const _,
            slot_config as *const _ as *const c_void,
            &mut card,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        // Register the SDMMC block device with lwext4
        let rc = lwext4_blockdev_init(card);
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        // Mount the EXT4 filesystem
        let rc = lwext4_mount(mp.as_ptr() as *const u8);
        if rc != 0 {
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        Ok(Ext4Mount {
            card,
            host_snapshot: *host,
            mount_point: mp,
        })
    }

    /// Mount an EXT4 filesystem that resides at a specific partition offset
    /// within the physical device.
    ///
    /// Use this when the disk has an MBR partition table and the EXT4
    /// partition does not start at sector 0.
    ///
    /// # Arguments
    /// * `host` / `slot_config` – SDMMC hardware descriptors.
    /// * `mount_point` – VFS path, e.g. `"/emmc"`.
    /// * `lba_start` – First sector (LBA) of the partition.
    /// * `sector_count` – Number of sectors in the partition.
    pub unsafe fn mount_at_partition(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &str,
        lba_start: u32,
        sector_count: u32,
    ) -> Result<Self, EmmcError> {
        let mp = CString::new(mount_point).map_err(|_| EmmcError::BadMountPoint)?;

        let mut card: *mut sdmmc_card_t = core::ptr::null_mut();
        let rc = lwext4_sdmmc_card_init(
            host as *const _,
            slot_config as *const _ as *const c_void,
            &mut card,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        let rc = lwext4_blockdev_init(card);
        if rc != ESP_OK as i32 {
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        let rc = lwext4_blockdev_set_partition(lba_start as u64, sector_count as u64);
        if rc != ESP_OK as i32 {
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        let rc = lwext4_mount(mp.as_ptr() as *const u8);
        if rc != 0 {
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        Ok(Ext4Mount {
            card,
            host_snapshot: *host,
            mount_point: mp,
        })
    }

    /// Mount the eMMC/SD card as EXT4, automatically scanning the MBR to
    /// find the correct partition offset.
    ///
    /// This is the preferred entry point when the disk may have an MBR
    /// partition table (i.e. the EXT4 filesystem does not start at sector 0).
    /// It initialises the card once, reads the MBR, and — if a Linux (type
    /// 0x83) partition is found — restricts the block-device window to that
    /// partition before mounting.  When no MBR entry is present the mount
    /// falls back to raw-device mode (sector 0).
    ///
    /// Returns the mounted filesystem together with the full MBR partition
    /// list so the caller can cache it without a second scan.
    ///
    /// # Safety
    /// Same requirements as [`mount`][Self::mount].
    pub unsafe fn mount_with_mbr_scan(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &str,
    ) -> Result<(Self, Vec<crate::PartitionInfo>), EmmcError> {
        let mp = CString::new(mount_point).map_err(|_| EmmcError::BadMountPoint)?;

        let mut card: *mut sdmmc_card_t = core::ptr::null_mut();
        let rc = lwext4_sdmmc_card_init(
            host as *const _,
            slot_config as *const _ as *const c_void,
            &mut card,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        // Scan partition table (MBR or GPT) before blockdev registration.
        // scan_partitions falls back to GPT automatically when the MBR holds
        // only a protective 0xEE entry (common on Linux-formatted eMMC).
        let partitions = crate::detect::scan_partitions(card);
        log::info!("[EXT4] partition scan: {} entr(ies) found", partitions.len());
        for (i, p) in partitions.iter().enumerate() {
            log::info!("[EXT4]   [{i}] type=0x{:02X} lba_start={} sectors={}",
                p.part_type, p.lba_start, p.sector_count);
        }
        let ext_part = partitions.iter().find(|p| p.is_ext()).cloned();
        if ext_part.is_none() {
            log::warn!("[EXT4] no Linux (0x83) partition found; will attempt raw-device mount");
        }

        let rc = lwext4_blockdev_init(card);
        if rc != ESP_OK as i32 {
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        // Restrict the block-device window to the EXT4 partition when found.
        if let Some(ref p) = ext_part {
            let rc = lwext4_blockdev_set_partition(p.lba_start as u64, p.sector_count as u64);
            if rc != ESP_OK as i32 {
                lwext4_blockdev_deinit();
                lwext4_sdmmc_card_deinit(host as *const _, card);
                return Err(EmmcError::EspError(rc));
            }
        }

        let rc = lwext4_mount(mp.as_ptr() as *const u8);
        if rc != 0 {
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        Ok((
            Ext4Mount { card, host_snapshot: *host, mount_point: mp },
            partitions,
        ))
    }

    /// Format the block device as EXT4, then mount it.
    ///
    /// Use this when the card does not yet have an EXT4 filesystem, or when
    /// you want a fresh filesystem for benchmarking.
    ///
    /// # Arguments
    /// * `label` – Optional volume label (≤ 16 chars).
    ///
    /// # Safety
    /// Same requirements as [`mount`][Self::mount].
    pub unsafe fn format_and_mount(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &str,
        label: Option<&str>,
    ) -> Result<Self, EmmcError> {
        let mp = CString::new(mount_point).map_err(|_| EmmcError::BadMountPoint)?;

        // Initialise the card
        let mut card: *mut sdmmc_card_t = core::ptr::null_mut();
        let rc = lwext4_sdmmc_card_init(
            host as *const _,
            slot_config as *const _ as *const c_void,
            &mut card,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        // Register block device
        let rc = lwext4_blockdev_init(card);
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }

        // Format as EXT4
        let label_c = label
            .map(|s| CString::new(s).unwrap_or_default());
        let label_ptr = label_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(core::ptr::null());
        let rc = lwext4_format(label_ptr);
        if rc != ESP_OK as i32 {
            lwext4_blockdev_deinit();
            return Err(EmmcError::EspError(rc));
        }

        // Mount
        let rc = lwext4_mount(mp.as_ptr() as *const u8);
        if rc != 0 {
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(host as *const _, card);
            return Err(EmmcError::EspError(rc));
        }

        Ok(Ext4Mount { card, host_snapshot: *host, mount_point: mp })
    }
}

impl Drop for Ext4Mount {
    fn drop(&mut self) {
        unsafe {
            lwext4_umount(self.mount_point.as_ptr() as *const u8);
            lwext4_blockdev_deinit();
            lwext4_sdmmc_card_deinit(&self.host_snapshot as *const _, self.card);
        }
    }
}
