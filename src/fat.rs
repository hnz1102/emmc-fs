/// FAT filesystem support via `esp_vfs_fat_sdmmc_mount`.
///
/// Mounting registers the FAT VFS at the chosen mount point so that standard
/// `std::fs` operations work transparently.  Formatting uses
/// `esp_vfs_fat_sdcard_format`.  Unmounting is performed automatically when
/// [`FatMount`] is dropped.
use crate::EmmcError;
use esp_idf_sys::{
    esp_vfs_fat_sdcard_format, esp_vfs_fat_sdmmc_mount, esp_vfs_fat_sdmmc_mount_config_t,
    esp_vfs_fat_sdmmc_unmount, sdmmc_card_t, sdmmc_host_t, sdmmc_slot_config_t, ESP_OK,
};
use std::ffi::{c_void, CString};

// ---------------------------------------------------------------------------
// FFI: fat_partition_mount / fat_partition_umount from fat_partition.c
// ---------------------------------------------------------------------------

extern "C" {
    /// Mount a FAT partition at a specific LBA offset using a custom diskio.
    fn fat_partition_mount(
        host: *const sdmmc_host_t,
        slot_config: *const c_void,
        mount_point: *const i8,
        lba_start: u64,
    ) -> i32;

    /// Unmount the FAT partition previously mounted by fat_partition_mount.
    fn fat_partition_umount(mount_point: *const i8) -> i32;
}

/// A mounted FAT filesystem.  Unmounts automatically on drop.
pub struct FatMount {
    /// Raw card pointer returned by `esp_vfs_fat_sdmmc_mount`.
    /// Null when using `mount_at_partition` (card managed by C code).
    pub card: *mut sdmmc_card_t,
    /// For standard mounts: null-terminated static byte slice.
    mount_point_static: Option<&'static [u8]>,
    /// For partition mounts: owned CString for unmount call.
    mount_point_owned: Option<CString>,
}

// The card pointer is only accessed from the thread that owns FatMount.
unsafe impl Send for FatMount {}

impl FatMount {
    /// Mount the eMMC/SD card as FAT at `mount_point`.
    ///
    /// # Arguments
    /// * `host` – SDMMC host descriptor (already configured).
    /// * `slot_config` – Slot pin / width configuration.
    /// * `mount_point` – Null-terminated path, e.g. `b"/eMMC\0"`.
    /// * `mount_config` – VFS-FAT mount options (max files, cluster size, …).
    ///
    /// # Safety
    /// All pointer arguments must remain valid for the lifetime of this mount.
    pub unsafe fn mount(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &'static [u8],
        mount_config: &esp_vfs_fat_sdmmc_mount_config_t,
    ) -> Result<Self, EmmcError> {
        let mut card: *mut sdmmc_card_t = core::ptr::null_mut();
        let rc = esp_vfs_fat_sdmmc_mount(
            mount_point.as_ptr(),
            host as *const _,
            slot_config as *const _ as *const c_void,
            mount_config as *const _,
            &mut card,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }
        Ok(FatMount { card, mount_point_static: Some(mount_point), mount_point_owned: None })
    }

    /// Mount a FAT partition that starts at a non-zero LBA offset.
    ///
    /// Uses a custom FATFS diskio driver that adds `lba_start` to every
    /// sector address, bypassing the broken GPT path in the standard
    /// `esp_vfs_fat_sdmmc_mount`.
    ///
    /// # Arguments
    /// * `host` / `slot_config` – SDMMC hardware descriptors.
    /// * `mount_point` – VFS path string, e.g. `"/emmc"`.
    /// * `lba_start` – First sector of the FAT partition.
    ///
    /// # Safety
    /// Same requirements as [`mount`][Self::mount].
    pub unsafe fn mount_at_partition(
        host: &sdmmc_host_t,
        slot_config: &sdmmc_slot_config_t,
        mount_point: &str,
        lba_start: u32,
    ) -> Result<Self, EmmcError> {
        let mp = CString::new(mount_point).map_err(|_| EmmcError::BadMountPoint)?;
        let rc = fat_partition_mount(
            host as *const _,
            slot_config as *const _ as *const c_void,
            mp.as_ptr() as *const i8,
            lba_start as u64,
        );
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }
        Ok(FatMount {
            card: core::ptr::null_mut(),
            mount_point_static: None,
            mount_point_owned: Some(mp),
        })
    }

    /// Re-format the card as FAT.  The filesystem must already be mounted.
    ///
    /// # Safety
    /// Must be called while the VFS mount is active.
    pub unsafe fn format(&self) -> Result<(), EmmcError> {
        let mp = self.mount_point_static.ok_or(EmmcError::BadMountPoint)?;
        let rc = esp_vfs_fat_sdcard_format(mp.as_ptr(), self.card);
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }
        Ok(())
    }
}

impl Drop for FatMount {
    fn drop(&mut self) {
        unsafe {
            if let Some(ref mp) = self.mount_point_owned {
                fat_partition_umount(mp.as_ptr() as *const i8);
            } else {
                esp_vfs_fat_sdmmc_unmount();
            }
        }
    }
}
