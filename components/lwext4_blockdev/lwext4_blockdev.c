/**
 * lwext4_blockdev.c
 *
 * Bridges the ESP32 SDMMC driver to the lwext4 block-device interface.
 * One block device named "emmc0" is registered with lwext4.
 */

#include "lwext4_blockdev.h"

#include "ext4.h"
#include "ext4_fs.h"
#include "ext4_blockdev.h"
#include "ext4_mkfs.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* Forward declarations for VFS helpers defined in lwext4_vfs.c */
esp_err_t lwext4_vfs_register(const char *mount_point);
esp_err_t lwext4_vfs_unregister(const char *mount_point);

static const char *TAG = "lwext4_blockdev";

/* -------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */

static sdmmc_card_t *s_card    = NULL;
static uint8_t      *s_dma_buf = NULL;   /* DMA-capable sector buffer */

#define SECTOR_SIZE   512u
#define DEVICE_NAME   "emmc0"

/* -------------------------------------------------------------------------
 * Write-coalescing buffer (DMA-capable internal RAM)
 *
 * lwext4's block cache evicts one physical block (8 sectors = 4 KB) at a
 * time.  Each eviction triggers an SDMMC write transaction that carries a
 * fixed ~1 ms programming overhead, limiting throughput to ~4 MiB/s.
 *
 * We buffer consecutive sector writes here and flush them as a single large
 * SDMMC transaction (up to 64 KB), reducing transaction count by 16× and
 * bringing EXT4 write speed close to the raw eMMC limit.
 *
 * IMPORTANT: the buffer MUST be in DMA-capable internal RAM.  PSRAM buffers
 * force sdmmc_write_sectors into a sector-by-sector bounce path that is
 * slower than no coalescing at all.
 * ---------------------------------------------------------------------- */
#define WBUF_SECTORS  128u                       /* 128 × 512 B = 64 KB */
#define WBUF_BYTES    (WBUF_SECTORS * SECTOR_SIZE)

static uint8_t  *s_wbuf     = NULL;  /* DMA-capable coalescing buffer      */
static uint64_t  s_wbuf_sec = 0;     /* sector LBA of first buffered entry */
static uint32_t  s_wbuf_cnt = 0;     /* number of 512-byte sectors buffered */

static int wbuf_flush(void)
{
    if (s_wbuf_cnt == 0) return EOK;
    esp_err_t rc = sdmmc_write_sectors(s_card, s_wbuf,
                                       (size_t)s_wbuf_sec,
                                       (size_t)s_wbuf_cnt);
    s_wbuf_cnt = 0;
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "wbuf_flush sdmmc_write_sectors failed: 0x%x", rc);
        return EIO;
    }
    return EOK;
}

/* -------------------------------------------------------------------------
 * lwext4 block-device interface callbacks
 * ---------------------------------------------------------------------- */

static int bdev_open(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

static int bdev_close(struct ext4_blockdev *bdev)
{
    (void)bdev;
    /* Flush any pending coalesced writes before the device is closed */
    return wbuf_flush();
}

static int bdev_bread(struct ext4_blockdev *bdev,
                      void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    (void)bdev;
    /* Flush coalesced writes that overlap the requested read range */
    if (s_wbuf_cnt > 0 &&
        blk_id              <  s_wbuf_sec + s_wbuf_cnt &&
        blk_id + blk_cnt    >  s_wbuf_sec) {
        int rc = wbuf_flush();
        if (rc != EOK) return rc;
    }

    size_t nbytes = (size_t)blk_cnt * SECTOR_SIZE;

    /* SDMMC DMA requires an internal-DRAM buffer.
     * lwext4's block cache uses plain malloc() which may return PSRAM.
     * When that happens, bounce through a DMA-capable buffer. */
    if (!esp_ptr_dma_capable(buf)) {
        ESP_LOGD(TAG, "bdev_bread: blk=%llu cnt=%u nbytes=%u — PSRAM buf %p, bouncing",
                 (unsigned long long)blk_id, (unsigned)blk_cnt, (unsigned)nbytes, buf);
        /* Prefer the 64-KB write buffer (DMA-capable); flush it first. */
        if (s_wbuf != NULL && nbytes <= WBUF_BYTES) {
            int frc = wbuf_flush();
            if (frc != EOK) return frc;
            esp_err_t rc = sdmmc_read_sectors(s_card, s_wbuf,
                                              (size_t)blk_id, (size_t)blk_cnt);
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "sdmmc_read_sectors (wbuf bounce) failed: 0x%x", rc);
                return EIO;
            }
            memcpy(buf, s_wbuf, nbytes);
            return EOK;
        }
        /* Fall back to the 512-byte DMA sector buffer, one sector at a time. */
        for (uint32_t i = 0; i < blk_cnt; i++) {
            esp_err_t rc = sdmmc_read_sectors(s_card, s_dma_buf,
                                              (size_t)(blk_id + i), 1);
            if (rc != ESP_OK) {
                ESP_LOGE(TAG, "sdmmc_read_sectors (dma_buf bounce) failed: 0x%x", rc);
                return EIO;
            }
            memcpy((uint8_t *)buf + (size_t)i * SECTOR_SIZE, s_dma_buf, SECTOR_SIZE);
        }
        return EOK;
    }

    esp_err_t rc = sdmmc_read_sectors(s_card, buf, (size_t)blk_id, (size_t)blk_cnt);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_read_sectors failed: 0x%x", rc);
        return EIO;
    }
    return EOK;
}

static int bdev_bwrite(struct ext4_blockdev *bdev,
                       const void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
    (void)bdev;

    if (s_wbuf == NULL) {
        /* No coalescing buffer: write directly */
        esp_err_t rc = sdmmc_write_sectors(s_card, buf,
                                           (size_t)blk_id, (size_t)blk_cnt);
        return rc == ESP_OK ? EOK : EIO;
    }

    /* Extend the current buffer if sectors are sequential */
    if (s_wbuf_cnt > 0 &&
        blk_id == s_wbuf_sec + s_wbuf_cnt &&
        s_wbuf_cnt + blk_cnt <= WBUF_SECTORS) {
        memcpy(s_wbuf + (size_t)s_wbuf_cnt * SECTOR_SIZE,
               buf, (size_t)blk_cnt * SECTOR_SIZE);
        s_wbuf_cnt += blk_cnt;
        /* Flush when full */
        return (s_wbuf_cnt >= WBUF_SECTORS) ? wbuf_flush() : EOK;
    }

    /* Non-sequential or overflow: flush what we have, then start fresh */
    int rc = wbuf_flush();
    if (rc != EOK) return rc;

    if (blk_cnt >= WBUF_SECTORS) {
        /* Too large for the coalescing buffer: write directly */
        esp_err_t err = sdmmc_write_sectors(s_card, buf,
                                            (size_t)blk_id, (size_t)blk_cnt);
        return err == ESP_OK ? EOK : EIO;
    }

    s_wbuf_sec = blk_id;
    memcpy(s_wbuf, buf, (size_t)blk_cnt * SECTOR_SIZE);
    s_wbuf_cnt = blk_cnt;
    return EOK;
}

/* Static lwext4 block-device instance (interface + device struct) */
EXT4_BLOCKDEV_STATIC_INSTANCE(
    s_bdev,
    SECTOR_SIZE,
    0,            /* ph_bcnt filled in lwext4_blockdev_init */
    bdev_open,
    bdev_bread,
    bdev_bwrite,
    bdev_close,
    NULL,         /* lock   */
    NULL          /* unlock */
);

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t lwext4_blockdev_init(sdmmc_card_t *card)
{
    if (card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_card = card;

    /* Allocate a DMA-capable sector buffer (required for ESP32 SDMMC DMA) */
    if (s_dma_buf == NULL) {
        s_dma_buf = heap_caps_malloc(SECTOR_SIZE,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_dma_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Allocate write-coalescing buffer from DMA-capable internal RAM (64 KB).
     * Must NOT be PSRAM: sdmmc_write_sectors requires a DMA-capable address. */
    if (s_wbuf == NULL) {
        s_wbuf = heap_caps_malloc(WBUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_wbuf == NULL) {
            ESP_LOGW(TAG, "No internal DMA RAM for write-coalescing buffer; writes uncoalesced");
        } else {
            ESP_LOGI(TAG, "Write-coalescing buffer: %u bytes in DMA-capable internal RAM", WBUF_BYTES);
        }
    }
    s_wbuf_sec = 0;
    s_wbuf_cnt = 0;

    /* Update block device geometry from card CSD */
    s_bdev.bdif->ph_bcnt = (uint64_t)card->csd.capacity;
    s_bdev.bdif->ph_bbuf = s_dma_buf;
    s_bdev.part_size     = (uint64_t)card->csd.capacity * SECTOR_SIZE;

    int rc = ext4_device_register(&s_bdev, DEVICE_NAME);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_device_register failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Block device '%s' registered — %llu sectors",
             DEVICE_NAME, (unsigned long long)s_bdev.bdif->ph_bcnt);
    return ESP_OK;
}

esp_err_t lwext4_blockdev_deinit(void)
{
    wbuf_flush();
    ext4_device_unregister(DEVICE_NAME);
    s_card = NULL;
    if (s_dma_buf != NULL) {
        free(s_dma_buf);
        s_dma_buf = NULL;
    }
    if (s_wbuf != NULL) {
        free(s_wbuf);
        s_wbuf = NULL;
    }
    return ESP_OK;
}

esp_err_t lwext4_blockdev_set_partition(uint64_t lba_start, uint64_t sector_count)
{
    s_bdev.part_offset = lba_start * (uint64_t)SECTOR_SIZE;
    if (sector_count > 0)
        s_bdev.part_size = sector_count * (uint64_t)SECTOR_SIZE;
    ESP_LOGI(TAG, "EXT4 partition window: lba_start=%llu size=%llu sectors",
             (unsigned long long)lba_start, (unsigned long long)sector_count);
    return ESP_OK;
}

esp_err_t lwext4_sdmmc_card_init(
    const sdmmc_host_t  *host,
    const void          *slot_config,
    sdmmc_card_t       **card_out)
{
    if (!host || !slot_config || !card_out) {
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_card_t *card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialise the SDMMC host peripheral */
    esp_err_t rc = host->init();
    if (rc != ESP_OK) {
        free(card);
        return rc;
    }

    /* Configure the slot pins / bus width */
    rc = sdmmc_host_init_slot(host->slot,
                              (const sdmmc_slot_config_t *)slot_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot failed: 0x%x", rc);
        host->deinit();
        free(card);
        return rc;
    }

    /* Probe and initialise the card */
    rc = sdmmc_card_init(host, card);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: 0x%x", rc);
        host->deinit();
        free(card);
        return rc;
    }

    sdmmc_card_print_info(stdout, card);
    *card_out = card;
    return ESP_OK;
}

void lwext4_sdmmc_card_deinit(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    if (host) {
        host->deinit();
    }
    free(card);
}

esp_err_t lwext4_format(const char *label)
{
    struct ext4_mkfs_info info;
    memset(&info, 0, sizeof(info));
    info.block_size = 4096;
    info.journal    = false;  /* disabled: no 3x write amplification */
    info.label      = label;  /* const char * — assign directly */

    /* ext4_mkfs requires a non-NULL fs struct — pass a stack-allocated one */
    struct ext4_fs tmp_fs;
    memset(&tmp_fs, 0, sizeof(tmp_fs));
    int rc = ext4_mkfs(&tmp_fs, &s_bdev, &info, F_SET_EXT4);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_mkfs failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "EXT4 format complete");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * EXT4 superblock diagnostics
 * ---------------------------------------------------------------------- */

/**
 * Read the EXT4 superblock and log the magic, incompat, and ro_compat
 * feature flags.  Highlights any incompat bits that are NOT in
 * CONFIG_SUPPORTED_FINCOM so the cause of ENOTSUP (errno 134) is visible.
 *
 * Superblock starts at byte offset 1024 from the partition start, which is
 * physical sector (part_offset / SECTOR_SIZE + 2).  Reuses the DMA-capable
 * s_dma_buf that has already been allocated by lwext4_blockdev_init.
 */
static void log_sb_features(void)
{
    if (s_card == NULL || s_dma_buf == NULL) return;

    uint64_t sb_lba = s_bdev.part_offset / SECTOR_SIZE + 2;
    esp_err_t rc = sdmmc_read_sectors(s_card, s_dma_buf, (size_t)sb_lba, 1);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "sb read at lba=%llu failed: 0x%x",
                 (unsigned long long)sb_lba, rc);
        return;
    }

    uint16_t magic = (uint16_t)s_dma_buf[0x38]
                   | ((uint16_t)s_dma_buf[0x39] << 8);
    uint32_t f_inc = (uint32_t)s_dma_buf[0x64]
                   | ((uint32_t)s_dma_buf[0x65] <<  8)
                   | ((uint32_t)s_dma_buf[0x66] << 16)
                   | ((uint32_t)s_dma_buf[0x67] << 24);
    uint32_t f_roc = (uint32_t)s_dma_buf[0x68]
                   | ((uint32_t)s_dma_buf[0x69] <<  8)
                   | ((uint32_t)s_dma_buf[0x6A] << 16)
                   | ((uint32_t)s_dma_buf[0x6B] << 24);

    ESP_LOGI(TAG, "EXT4 superblock: magic=0x%04X incompat=0x%08lX ro_compat=0x%08lX",
             magic, (unsigned long)f_inc, (unsigned long)f_roc);

    if (magic != 0xEF53) {
        ESP_LOGW(TAG, "  magic mismatch — superblock not found at expected location");
        return;
    }

    uint32_t unsupported = f_inc & ~(uint32_t)CONFIG_SUPPORTED_FINCOM;
    if (unsupported) {
        ESP_LOGW(TAG, "  unsupported incompat bits: 0x%08lX",
                 (unsigned long)unsupported);
        if (unsupported & EXT4_FINCOM_COMPRESSION)
            ESP_LOGW(TAG, "    0x0001 COMPRESSION");
        if (unsupported & EXT4_FINCOM_JOURNAL_DEV)
            ESP_LOGW(TAG, "    0x0008 JOURNAL_DEV");
        if (unsupported & EXT4_FINCOM_EA_INODE)
            ESP_LOGW(TAG, "    0x0400 EA_INODE");
        if (unsupported & EXT4_FINCOM_DIRDATA)
            ESP_LOGW(TAG, "    0x1000 DIRDATA");
        if (unsupported & EXT4_FINCOM_BG_USE_META_CSUM)
            ESP_LOGW(TAG, "    0x2000 BG_USE_META_CSUM (csum_seed)");
        if (unsupported & EXT4_FINCOM_LARGEDIR)
            ESP_LOGW(TAG, "    0x4000 LARGEDIR");
        if (unsupported & EXT4_FINCOM_INLINE_DATA)
            ESP_LOGW(TAG, "    0x8000 INLINE_DATA");
    } else {
        ESP_LOGI(TAG, "  all incompat features are in supported/ignored set");
    }
}

esp_err_t lwext4_mount(const char *mount_point)
{
    if (mount_point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* lwext4 requires the mount point to end with '/' */
    char mp_slash[CONFIG_EXT4_MAX_MP_NAME + 2];
    size_t len = strlen(mount_point);
    if (len + 2 > sizeof(mp_slash)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(mp_slash, mount_point, len);
    if (mp_slash[len - 1] != '/') {
        mp_slash[len++] = '/';
    }
    mp_slash[len] = '\0';

    int rc = ext4_mount(DEVICE_NAME, mp_slash, false /* read-write */);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_mount('%s') failed: %d", mp_slash, rc);
        log_sb_features();  /* diagnose which incompat feature caused ENOTSUP */
        return ESP_FAIL;
    }

    /* Register the EXT4 filesystem with the ESP-IDF VFS so that std::fs works */
    esp_err_t vfs_rc = lwext4_vfs_register(mount_point);
    if (vfs_rc != ESP_OK) {
        ESP_LOGW(TAG, "VFS registration failed: 0x%x (file I/O via lwext4 directly)", vfs_rc);
    }

    ESP_LOGI(TAG, "EXT4 mounted at '%s'", mp_slash);
    return ESP_OK;
}

esp_err_t lwext4_umount(const char *mount_point)
{
    if (mount_point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build trailing-slash version for lwext4 internal API */
    char mp_slash[CONFIG_EXT4_MAX_MP_NAME + 2];
    size_t len = strlen(mount_point);
    if (len + 2 > sizeof(mp_slash)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(mp_slash, mount_point, len);
    if (mp_slash[len - 1] != '/') {
        mp_slash[len++] = '/';
    }
    mp_slash[len] = '\0';

    lwext4_vfs_unregister(mount_point);

    int rc = ext4_umount(mp_slash);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_umount('%s') failed: %d", mp_slash, rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "EXT4 unmounted from '%s'", mp_slash);
    return ESP_OK;
}
