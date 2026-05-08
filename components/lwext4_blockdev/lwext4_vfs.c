/**
 * lwext4_vfs.c
 *
 * Registers lwext4 with the ESP-IDF Virtual FileSystem (VFS) so that
 * standard POSIX / Rust std::fs operations work on EXT4 partitions.
 *
 * Supported operations: open, close, read, write, lseek, fstat, stat,
 * unlink, rename, mkdir, rmdir, opendir, readdir, closedir.
 */

#include "lwext4_blockdev.h"

#include "ext4.h"

#include "esp_vfs.h"
#include "esp_log.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "lwext4_vfs";

/* -------------------------------------------------------------------------
 * Open-file table
 * ---------------------------------------------------------------------- */

#define MAX_OPEN_FILES  8

typedef struct {
    bool      in_use;
    ext4_file fh;
    char      path[256];
} vfs_fd_entry_t;

static vfs_fd_entry_t s_fds[MAX_OPEN_FILES];

static int alloc_fd(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!s_fds[i].in_use) {
            s_fds[i].in_use = true;
            return i;
        }
    }
    return -1;
}

static void free_fd(int fd)
{
    if (fd >= 0 && fd < MAX_OPEN_FILES) {
        s_fds[fd].in_use = false;
    }
}

/* -------------------------------------------------------------------------
 * Mount-point prefix bookkeeping
 * ---------------------------------------------------------------------- */

static char s_mount_point[64] = {0};

/* Convert a VFS-relative path (without the mount-point prefix) to a full
 * path expected by lwext4, e.g. "/ext4/foo.txt".
 * For directory operations, use full_dir_path() which appends a trailing '/'. */
static const char *full_path(const char *rel)
{
    /* VFS strips the mount prefix, so rel starts with '/'.  lwext4 needs the
     * full absolute path including the mount point. */
    static char buf[320];
    snprintf(buf, sizeof(buf), "%s%s", s_mount_point, rel);
    return buf;
}

/* Like full_path() but ensures a trailing '/' — required by lwext4 for
 * directory operations (ext4_dir_open, ext4_dir_mk, ext4_dir_rm). */
static const char *full_dir_path(const char *rel)
{
    static char buf[322];
    int len = snprintf(buf, sizeof(buf) - 2, "%s%s", s_mount_point, rel);
    if (len > 0 && buf[len - 1] != '/') {
        buf[len]     = '/';
        buf[len + 1] = '\0';
    }
    return buf;
}

/* -------------------------------------------------------------------------
 * VFS callbacks — files
 * ---------------------------------------------------------------------- */

static int vfs_open(const char *path, int flags, int mode)
{
    (void)mode;

    const char *ext4_flags;
    if ((flags & O_ACCMODE) == O_RDONLY) {
        ext4_flags = "r";
    } else if ((flags & O_ACCMODE) == O_WRONLY) {
        ext4_flags = (flags & O_APPEND) ? "a" : (flags & O_TRUNC) ? "w" : "w";
    } else { /* O_RDWR */
        ext4_flags = (flags & O_CREAT) ? "w+" : "r+";
    }

    int fd = alloc_fd();
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }

    int rc = ext4_fopen(&s_fds[fd].fh, full_path(path), ext4_flags);
    if (rc != EOK) {
        free_fd(fd);
        errno = rc;
        return -1;
    }

    strncpy(s_fds[fd].path, full_path(path), sizeof(s_fds[fd].path) - 1);
    return fd;
}

static int vfs_close(int fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_fds[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    ext4_fclose(&s_fds[fd].fh);
    free_fd(fd);
    return 0;
}

static ssize_t vfs_read(int fd, void *dst, size_t size)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_fds[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    size_t rcnt = 0;
    int rc = ext4_fread(&s_fds[fd].fh, dst, size, &rcnt);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return (ssize_t)rcnt;
}

static ssize_t vfs_write(int fd, const void *src, size_t size)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_fds[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    size_t wcnt = 0;
    int rc = ext4_fwrite(&s_fds[fd].fh, src, size, &wcnt);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return (ssize_t)wcnt;
}

static off_t vfs_lseek(int fd, off_t offset, int whence)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_fds[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    int rc = ext4_fseek(&s_fds[fd].fh, (int64_t)offset, whence);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return (off_t)ext4_ftell(&s_fds[fd].fh);
}

static int vfs_fstat(int fd, struct stat *st)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_fds[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)ext4_fsize(&s_fds[fd].fh);
    st->st_mode = S_IFREG | 0666;
    return 0;
}

static int vfs_stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));

    /* Try opening as a regular file first */
    ext4_file f;
    int rc = ext4_fopen(&f, full_path(path), "r");
    if (rc == EOK) {
        st->st_size = (off_t)ext4_fsize(&f);
        st->st_mode = S_IFREG | 0644;
        ext4_fclose(&f);
        return 0;
    }

    /* Fall back to checking as a directory — lwext4 needs trailing '/' */
    ext4_dir d;
    rc = ext4_dir_open(&d, full_dir_path(path));
    if (rc == EOK) {
        st->st_mode = S_IFDIR | 0755;
        ext4_dir_close(&d);
        return 0;
    }

    errno = ENOENT;
    return -1;
}

static int vfs_unlink(const char *path)
{
    int rc = ext4_fremove(full_path(path));
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_rename(const char *src, const char *dst)
{
    int rc = ext4_frename(full_path(src), full_path(dst));
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    int rc = ext4_dir_mk(full_dir_path(path));
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_rmdir(const char *path)
{
    int rc = ext4_dir_rm(full_dir_path(path));
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

/* -------------------------------------------------------------------------
 * VFS callbacks — directories
 * ---------------------------------------------------------------------- */

/* The ESP-IDF VFS layer writes dd_vfs_idx (uint16_t) + dd_rsv (uint16_t)
 * into the first 4 bytes of the DIR* returned by opendir.  If we put our
 * own data (ext4_dir, whose first field is the mp pointer) at offset 0 it
 * gets corrupted.  The fix, identical to ESP-IDF's own fatfs VFS, is to
 * embed the standard DIR struct as the FIRST member so that ESP-IDF writes
 * into the right place and our ext4 state begins at offset sizeof(DIR). */
typedef struct {
    DIR         base;       /* must be first — ESP-IDF VFS writes dd_vfs_idx here */
    ext4_dir    dir;
    bool        in_use;
} vfs_dir_entry_t;

#define MAX_OPEN_DIRS  4
static vfs_dir_entry_t s_dirs[MAX_OPEN_DIRS];

static DIR *vfs_opendir(const char *path)
{
    int i;
    for (i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!s_dirs[i].in_use) break;
    }
    if (i == MAX_OPEN_DIRS) {
        errno = ENFILE;
        return NULL;
    }
    const char *dp = full_dir_path(path);
    int rc = ext4_dir_open(&s_dirs[i].dir, dp);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_dir_open('%s') failed: %d", dp, rc);
        errno = rc;
        return NULL;
    }
    s_dirs[i].in_use = true;
    return (DIR *)&s_dirs[i];
}

static struct dirent *vfs_readdir(DIR *pdir)
{
    vfs_dir_entry_t *d = (vfs_dir_entry_t *)pdir;
    const ext4_direntry *de;

    /* Skip deleted (inode=0) entries */
    do {
        de = ext4_dir_entry_next(&d->dir);
        if (de == NULL) return NULL;
    } while (de->inode == 0);

    static struct dirent ent;
    memset(&ent, 0, sizeof(ent));
    uint8_t nlen = de->name_length < sizeof(ent.d_name) - 1
                 ? de->name_length : (uint8_t)(sizeof(ent.d_name) - 1);
    memcpy(ent.d_name, de->name, nlen);
    ent.d_name[nlen] = '\0';
    ent.d_type = (de->inode_type == EXT4_DE_DIR) ? DT_DIR : DT_REG;
    return &ent;
}

static int vfs_closedir(DIR *pdir)
{
    vfs_dir_entry_t *d = (vfs_dir_entry_t *)pdir;
    ext4_dir_close(&d->dir);
    d->in_use = false;
    return 0;
}

static int vfs_readdir_r(DIR *pdir, struct dirent *entry, struct dirent **out)
{
    struct dirent *de = vfs_readdir(pdir);
    if (de == NULL) {
        *out = NULL;
        return 0;    /* end of directory — not an error */
    }
    memcpy(entry, de, sizeof(struct dirent));
    *out = entry;
    return 0;
}

/* -------------------------------------------------------------------------
 * VFS registration (called from lwext4_mount in lwext4_blockdev.c)
 * ---------------------------------------------------------------------- */

esp_err_t lwext4_vfs_register(const char *mount_point)
{
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);

    static const esp_vfs_t vfs = {
        .flags      = ESP_VFS_FLAG_DEFAULT,
        .open       = &vfs_open,
        .close      = &vfs_close,
        .read       = &vfs_read,
        .write      = &vfs_write,
        .lseek      = &vfs_lseek,
        .fstat      = &vfs_fstat,
        .stat       = &vfs_stat,
        .unlink     = &vfs_unlink,
        .rename     = &vfs_rename,
        .mkdir      = &vfs_mkdir,
        .rmdir      = &vfs_rmdir,
        .opendir    = &vfs_opendir,
        .readdir    = &vfs_readdir,
        .readdir_r  = &vfs_readdir_r,
        .closedir   = &vfs_closedir,
    };

    esp_err_t rc = esp_vfs_register(mount_point, &vfs, NULL);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register('%s') failed: 0x%x", mount_point, rc);
        return rc;
    }

    ESP_LOGI(TAG, "EXT4 VFS registered at '%s'", mount_point);
    return ESP_OK;
}

esp_err_t lwext4_vfs_unregister(const char *mount_point)
{
    esp_err_t rc = esp_vfs_unregister(mount_point);
    s_mount_point[0] = '\0';
    return rc;
}
