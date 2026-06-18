/**
 * fat_partition.c
 *
 * Mount a FAT partition that starts at a non-zero LBA offset on the physical
 * device (GPT EFI System Partition, Microsoft Basic Data, …).
 *
 * The standard esp_vfs_fat_sdmmc_mount() lets the FATFS library read sector 0
 * to find MBR partition entries.  On a GPT disk sector 0 is the protective
 * MBR (type 0xEE) which FATFS cannot mount, so that API always fails for
 * GPT-based FAT partitions.
 *
 * This module works around the limitation by registering a custom FATFS diskio
 * driver that adds `lba_start` to every sector request, making the partition's
 * Volume Boot Record (VBR) appear at logical sector 0.  DMA constraints are
 * handled with a bounce buffer identical to the one in lwext4_blockdev.c.
 */

#include "lwext4_blockdev.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG_FAT = "fat_partition";

/* -------------------------------------------------------------------------
 * Module state (supports one concurrent FAT-partition mount)
 * ---------------------------------------------------------------------- */

#define FAT_BOUNCE_SECTORS 8u
#define FAT_SECTOR_SIZE    512u

static sdmmc_card_t *s_fat_card    = NULL;
static uint64_t      s_fat_lba_off = 0;
static BYTE          s_fat_pdrv    = 0xFF;
static uint8_t      *s_fat_bounce  = NULL;
static sdmmc_host_t  s_fat_host;
static char          s_fat_mp[64]; /* mount point copy for unmount */

/* -------------------------------------------------------------------------
 * Custom FATFS diskio implementation
 * ---------------------------------------------------------------------- */

static DSTATUS fat_part_init_cb(BYTE pdrv)   { (void)pdrv; return 0; }
static DSTATUS fat_part_status_cb(BYTE pdrv) { (void)pdrv; return 0; }

static DRESULT fat_part_read(BYTE pdrv, BYTE *buf, DWORD sector, UINT count)
{
    (void)pdrv;
    size_t lba = (size_t)(s_fat_lba_off + sector);

    if (esp_ptr_dma_capable(buf)) {
        esp_err_t rc = sdmmc_read_sectors(s_fat_card, buf, lba, count);
        return (rc == ESP_OK) ? RES_OK : RES_ERROR;
    }

    /* Bounce through DMA-capable buffer */
    DRESULT res = RES_OK;
    size_t done = 0;
    while (done < count) {
        UINT batch = (UINT)((count - done < FAT_BOUNCE_SECTORS)
                            ? count - done : FAT_BOUNCE_SECTORS);
        esp_err_t rc = sdmmc_read_sectors(s_fat_card, s_fat_bounce,
                                          lba + done, batch);
        if (rc != ESP_OK) { res = RES_ERROR; break; }
        memcpy(buf + done * FAT_SECTOR_SIZE, s_fat_bounce,
               batch * FAT_SECTOR_SIZE);
        done += batch;
    }
    return res;
}

static DRESULT fat_part_write(BYTE pdrv, const BYTE *buf, DWORD sector, UINT count)
{
    (void)pdrv;
    size_t lba = (size_t)(s_fat_lba_off + sector);

    if (esp_ptr_dma_capable(buf)) {
        esp_err_t rc = sdmmc_write_sectors(s_fat_card, buf, lba, count);
        return (rc == ESP_OK) ? RES_OK : RES_ERROR;
    }

    DRESULT res = RES_OK;
    size_t done = 0;
    while (done < count) {
        UINT batch = (UINT)((count - done < FAT_BOUNCE_SECTORS)
                            ? count - done : FAT_BOUNCE_SECTORS);
        memcpy(s_fat_bounce, buf + done * FAT_SECTOR_SIZE,
               batch * FAT_SECTOR_SIZE);
        esp_err_t rc = sdmmc_write_sectors(s_fat_card, s_fat_bounce,
                                           lba + done, batch);
        if (rc != ESP_OK) { res = RES_ERROR; break; }
        done += batch;
    }
    return res;
}

static DRESULT fat_part_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buf = FAT_SECTOR_SIZE;
            return RES_OK;
        case GET_SECTOR_COUNT:
            /* Report only the sectors in this partition. */
            *(DWORD *)buf = (DWORD)(s_fat_card->csd.capacity - s_fat_lba_off);
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buf = 8;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_fat_diskio = {
    .init   = fat_part_init_cb,
    .status = fat_part_status_cb,
    .read   = fat_part_read,
    .write  = fat_part_write,
    .ioctl  = fat_part_ioctl,
};

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t fat_partition_mount(
    const sdmmc_host_t *host,
    const void         *slot_config,
    const char         *mount_point,
    uint64_t            lba_start)
{
    esp_err_t rc;

    /* Allocate DMA-capable bounce buffer */
    if (s_fat_bounce == NULL) {
        s_fat_bounce = heap_caps_malloc(
            FAT_BOUNCE_SECTORS * FAT_SECTOR_SIZE,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_fat_bounce == NULL) {
            ESP_LOGE(TAG_FAT, "No DMA RAM for bounce buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Init SDMMC card */
    sdmmc_card_t *card = malloc(sizeof(sdmmc_card_t));
    if (!card) return ESP_ERR_NO_MEM;

    rc = host->init();
    if (rc != ESP_OK) { free(card); return rc; }

    rc = sdmmc_host_init_slot(host->slot,
                              (const sdmmc_slot_config_t *)slot_config);
    if (rc != ESP_OK) { host->deinit(); free(card); return rc; }

    rc = sdmmc_card_init(host, card);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG_FAT, "sdmmc_card_init failed: 0x%x", rc);
        host->deinit(); free(card); return rc;
    }

    s_fat_card    = card;
    s_fat_lba_off = lba_start;
    s_fat_host    = *host;
    strncpy(s_fat_mp, mount_point, sizeof(s_fat_mp) - 1);
    s_fat_mp[sizeof(s_fat_mp) - 1] = '\0';

    /* Get a free FATFS physical drive number */
    BYTE pdrv = 0xFF;
    rc = ff_diskio_get_drive(&pdrv);
    if (rc != ESP_OK || pdrv == 0xFF) {
        ESP_LOGE(TAG_FAT, "No free FATFS drive number");
        host->deinit(); free(card); return ESP_ERR_NOT_FOUND;
    }
    s_fat_pdrv = pdrv;

    /* Register our partition-aware diskio */
    ff_diskio_register(pdrv, &s_fat_diskio);

    /* Register the VFS path and mount */
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    FATFS *fs = NULL;
    rc = esp_vfs_fat_register(mount_point, drv, 8 /*max_files*/, &fs);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG_FAT, "esp_vfs_fat_register failed: 0x%x", rc);
        goto fail_diskio;
    }

    FRESULT frc = f_mount(fs, drv, 1 /* mount now */);
    if (frc != FR_OK) {
        ESP_LOGE(TAG_FAT, "f_mount failed: %d", (int)frc);
        esp_vfs_fat_unregister_path(mount_point);
        rc = ESP_FAIL;
        goto fail_diskio;
    }

    ESP_LOGI(TAG_FAT, "FAT partition mounted at '%s' (lba_start=%llu)",
             mount_point, (unsigned long long)lba_start);
    return ESP_OK;

fail_diskio:
    ff_diskio_unregister(pdrv);
    s_fat_pdrv = 0xFF;
    host->deinit();
    free(s_fat_card);
    s_fat_card = NULL;
    return rc;
}

esp_err_t fat_partition_umount(const char *mount_point)
{
    if (s_fat_pdrv == 0xFF) return ESP_OK;

    char drv[3] = { (char)('0' + s_fat_pdrv), ':', 0 };
    f_mount(NULL, drv, 0);                      /* unmount FATFS volume */
    esp_vfs_fat_unregister_path(mount_point);   /* unregister VFS path  */
    ff_diskio_unregister(s_fat_pdrv);
    s_fat_pdrv = 0xFF;

    s_fat_host.deinit();
    free(s_fat_card);
    s_fat_card = NULL;

    if (s_fat_bounce) {
        free(s_fat_bounce);
        s_fat_bounce = NULL;
    }

    ESP_LOGI(TAG_FAT, "FAT partition unmounted from '%s'", mount_point);
    return ESP_OK;
}
