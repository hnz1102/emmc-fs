#pragma once

#include "sdmmc_cmd.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the lwext4 SDMMC block device.
 *
 * Must be called after the card has been initialised (e.g. after
 * sdmmc_card_init / esp_vfs_fat_sdmmc_mount).
 *
 * @param card  Pointer to the initialised sdmmc_card_t.
 * @return ESP_OK on success.
 */
esp_err_t lwext4_blockdev_init(sdmmc_card_t *card);

/**
 * @brief Release the lwext4 block device registration.
 * @return ESP_OK on success.
 */
esp_err_t lwext4_blockdev_deinit(void);

/**
 * @brief Restrict block-device access to a specific MBR partition window.
 *
 * Must be called AFTER lwext4_blockdev_init() and BEFORE lwext4_mount().
 * Sets the byte offset and size of the partition within the physical device so
 * that lwext4 sees only the partition, not the whole disk.
 *
 * @param lba_start    First LBA (logical block address) of the partition.
 * @param sector_count Number of 512-byte sectors in the partition.
 * @return ESP_OK on success.
 */
esp_err_t lwext4_blockdev_set_partition(uint64_t lba_start, uint64_t sector_count);

/**
 * @brief Initialise the SDMMC card via the given host + slot configuration.
 *
 * Thin wrapper around sdmmc_card_init that allocates and populates the card
 * struct.  The caller is responsible for freeing it with free().
 *
 * @param host        SDMMC host descriptor.
 * @param slot_config Slot pin / width configuration (cast to void *).
 * @param card_out    Receives the allocated sdmmc_card_t pointer.
 * @return ESP_OK on success.
 */
esp_err_t lwext4_sdmmc_card_init(
    const sdmmc_host_t   *host,
    const void           *slot_config,
    sdmmc_card_t        **card_out);

/**
 * @brief Mount an EXT4 filesystem and register it with the ESP-IDF VFS.
 *
 * After a successful call, standard POSIX/Rust std::fs paths under
 * mount_point will use the EXT4 filesystem.
 *
 * @param mount_point  Null-terminated path (e.g. "/ext4").
 * @return ESP_OK on success.
 */
esp_err_t lwext4_mount(const char *mount_point);

/**
 * @brief Unmount the EXT4 filesystem at mount_point.
 *
 * @param mount_point  Must match the path passed to lwext4_mount().
 * @return ESP_OK on success.
 */
esp_err_t lwext4_umount(const char *mount_point);

/**
 * @brief Format the registered block device as EXT4.
 *
 * Must be called after lwext4_blockdev_init() but before lwext4_mount().
 * The block device must NOT be mounted when this is called.
 *
 * @param label  Optional volume label (up to 16 chars), or NULL.
 * @return ESP_OK on success.
 */
esp_err_t lwext4_format(const char *label);

/**
 * @brief Release resources allocated by lwext4_sdmmc_card_init.
 *
 * Calls host->deinit() and frees the card struct.
 *
 * @param host  Same host descriptor used in lwext4_sdmmc_card_init.
 * @param card  Card pointer returned by lwext4_sdmmc_card_init.
 */
void lwext4_sdmmc_card_deinit(const sdmmc_host_t *host, sdmmc_card_t *card);

/**
 * @brief Register the EXT4 filesystem with the ESP-IDF VFS.
 *
 * Called automatically by lwext4_mount().  Provides POSIX-compatible
 * file I/O (open/read/write/…) so that Rust std::fs works on EXT4 paths.
 *
 * @param mount_point  Must match the path passed to lwext4_mount().
 * @return ESP_OK on success.
 */
esp_err_t lwext4_vfs_register(const char *mount_point);

/**
 * @brief Unregister the EXT4 VFS driver.
 *
 * Called automatically by lwext4_umount().
 */
esp_err_t lwext4_vfs_unregister(const char *mount_point);

/**
 * @brief Recursively remove a directory and all its contents.
 *
 * Calls ext4_dir_rm() on the given full VFS path (e.g. "/emmc/mydir").
 * ext4_dir_rm() handles the recursion internally at the ext4 level,
 * so callers must NOT remove child entries beforehand.
 *
 * @param path  Full VFS path of the directory to remove.
 * @return 0 on success, or a positive errno value on failure.
 */
int lwext4_rmdir_recursive(const char *path);

#ifdef __cplusplus
}
#endif
