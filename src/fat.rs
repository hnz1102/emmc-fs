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
use std::ffi::c_void;

/// A mounted FAT filesystem.  Unmounts automatically on drop.
pub struct FatMount {
    /// Raw card pointer returned by `esp_vfs_fat_sdmmc_mount`.
    pub card: *mut sdmmc_card_t,
    mount_point: &'static [u8], // null-terminated byte string
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
        Ok(FatMount { card, mount_point })
    }

    /// Re-format the card as FAT.  The filesystem must already be mounted.
    ///
    /// # Safety
    /// Must be called while the VFS mount is active.
    pub unsafe fn format(&self) -> Result<(), EmmcError> {
        let rc = esp_vfs_fat_sdcard_format(self.mount_point.as_ptr(), self.card);
        if rc != ESP_OK as i32 {
            return Err(EmmcError::EspError(rc));
        }
        Ok(())
    }
}

impl Drop for FatMount {
    fn drop(&mut self) {
        unsafe {
            esp_vfs_fat_sdmmc_unmount();
        }
    }
}
