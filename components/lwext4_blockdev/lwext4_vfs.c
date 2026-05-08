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
#include <stdio.h>

static const char *TAG = "lwext4_vfs";

#define VFS_PATH_BUF_SZ 320
#define MAX_SYMLINK_DEPTH 8

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

static void build_full_path(char *dst, size_t dst_sz, const char *rel)
{
    snprintf(dst, dst_sz, "%s%s", s_mount_point, rel);
}

static void build_full_dir_path(char *dst, size_t dst_sz, const char *rel)
{
    int len = snprintf(dst, dst_sz, "%s%s", s_mount_point, rel);
    if (len > 0 && (size_t)len < dst_sz - 1 && dst[len - 1] != '/') {
        dst[len]     = '/';
        dst[len + 1] = '\0';
    }
}

/*
 * resolve_symlink_path — resolve symlinks component by component.
 *
 * ext4_mode_get() does not recognise the mount point itself ("/emmc"), so
 * we initialise 'resolved' to the mount-point string and start processing
 * 'remaining' from the path after the mount point.  This avoids passing
 * the "/emmc" component to ext4_mode_get and allows intermediate symlinks
 * to be followed correctly.
 *
 * Example: /emmc/link_dir/file.txt where link_dir is a symlink
 *          → resolves to /emmc/real_dir/file.txt
 */
static int resolve_symlink_path(const char *in_path, char *out_path, size_t out_sz)
{
    char resolved[VFS_PATH_BUF_SZ];
    char remaining[VFS_PATH_BUF_SZ];

    /* Skip the mount-point prefix so ext4_mode_get never sees it.
     * Initialise resolved = s_mount_point; remaining = path after the prefix. */
    size_t mp_len = strlen(s_mount_point);
    if (mp_len > 0
        && strncmp(in_path, s_mount_point, mp_len) == 0
        && (in_path[mp_len] == '/' || in_path[mp_len] == '\0')) {
        strncpy(resolved, s_mount_point, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
        strncpy(remaining, in_path + mp_len, sizeof(remaining) - 1);
        remaining[sizeof(remaining) - 1] = '\0';
    } else {
        /* No mount-point prefix — seed resolved with the leading '/'. */
        if (in_path[0] == '/') {
            resolved[0] = '/';
            resolved[1] = '\0';
        } else {
            resolved[0] = '\0';
        }
        strncpy(remaining, in_path, sizeof(remaining) - 1);
        remaining[sizeof(remaining) - 1] = '\0';
    }

    int total_symlinks = 0;

    while (1) {
        /* Skip leading slashes in remaining. */
        char *p = remaining;
        while (*p == '/') p++;
        if (*p == '\0') break; /* all components processed */

        /* Extract the next path component. */
        char *slash = strchr(p, '/');
        char component[256];
        char rest[VFS_PATH_BUF_SZ];

        if (slash) {
            size_t clen = (size_t)(slash - p);
            if (clen >= sizeof(component)) clen = sizeof(component) - 1;
            memcpy(component, p, clen);
            component[clen] = '\0';
            strncpy(rest, slash, sizeof(rest) - 1);
            rest[sizeof(rest) - 1] = '\0';
        } else {
            strncpy(component, p, sizeof(component) - 1);
            component[sizeof(component) - 1] = '\0';
            rest[0] = '\0';
        }

        /* Build the candidate path: resolved + component. */
        char candidate[VFS_PATH_BUF_SZ];
        size_t rlen = strlen(resolved);
        if (rlen > 0 && resolved[rlen - 1] == '/') {
            snprintf(candidate, sizeof(candidate), "%s%s", resolved, component);
        } else if (rlen == 0) {
            snprintf(candidate, sizeof(candidate), "%s", component);
        } else {
            snprintf(candidate, sizeof(candidate), "%s/%s", resolved, component);
        }

        /* Check the type of this component. */
        uint32_t mode = 0;
        int rc = ext4_mode_get(candidate, &mode);
        if (rc != EOK) {
            /* Path does not exist yet (e.g. new file creation) —
             * append the remainder and return EOK so the caller can
             * report the real error from ext4_fopen / ext4_dir_open. */
            if (rest[0]) {
                snprintf(out_path, out_sz, "%s%s", candidate, rest);
            } else {
                strncpy(out_path, candidate, out_sz - 1);
                out_path[out_sz - 1] = '\0';
            }
            return EOK;
        }

        if ((mode & S_IFMT) == S_IFLNK) {
            if (++total_symlinks >= MAX_SYMLINK_DEPTH) return ELOOP;

            /* Read the symlink target. */
            char target[VFS_PATH_BUF_SZ];
            size_t rcnt = 0;
            rc = ext4_readlink(candidate, target, sizeof(target) - 1, &rcnt);
            if (rc != EOK) return rc;
            target[rcnt] = '\0';

            char new_remaining[VFS_PATH_BUF_SZ];
            snprintf(new_remaining, sizeof(new_remaining), "%s%s", target, rest);

            if (target[0] == '/') {
                /* Absolute symlink: reset resolved.
                 * If the target is inside the mount point, reset resolved to
                 * the mount-point string and strip the prefix from remaining
                 * so ext4_mode_get never sees it.  Otherwise reset to '/'. */
                if (mp_len > 0
                    && strncmp(target, s_mount_point, mp_len) == 0
                    && (target[mp_len] == '/' || target[mp_len] == '\0')) {
                    strncpy(resolved, s_mount_point, sizeof(resolved) - 1);
                    resolved[sizeof(resolved) - 1] = '\0';
                    char adjusted[VFS_PATH_BUF_SZ];
                    snprintf(adjusted, sizeof(adjusted), "%s%s", target + mp_len, rest);
                    strncpy(new_remaining, adjusted, sizeof(new_remaining) - 1);
                    new_remaining[sizeof(new_remaining) - 1] = '\0';
                } else {
                    resolved[0] = '/';
                    resolved[1] = '\0';
                }
            }
            /* Relative symlink: keep resolved (parent directory of the link). */

            strncpy(remaining, new_remaining, sizeof(remaining) - 1);
            remaining[sizeof(remaining) - 1] = '\0';
            continue;
        }

        /* Not a symlink — advance resolved and move to the next component. */
        strncpy(resolved, candidate, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';

        strncpy(remaining, rest, sizeof(remaining) - 1);
        remaining[sizeof(remaining) - 1] = '\0';
    }

    strncpy(out_path, resolved, out_sz - 1);
    out_path[out_sz - 1] = '\0';
    return EOK;
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

    char raw_path[VFS_PATH_BUF_SZ];
    char real_path[VFS_PATH_BUF_SZ];
    build_full_path(raw_path, sizeof(raw_path), path);

    int rc = resolve_symlink_path(raw_path, real_path, sizeof(real_path));
    if (rc != EOK) {
        free_fd(fd);
        errno = rc;
        return -1;
    }

    rc = ext4_fopen(&s_fds[fd].fh, real_path, ext4_flags);
    if (rc != EOK) {
        free_fd(fd);
        errno = rc;
        return -1;
    }

    strncpy(s_fds[fd].path, real_path, sizeof(s_fds[fd].path) - 1);
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

    char raw_path[VFS_PATH_BUF_SZ];
    char real_path[VFS_PATH_BUF_SZ];
    build_full_path(raw_path, sizeof(raw_path), path);

    int rc = resolve_symlink_path(raw_path, real_path, sizeof(real_path));
    if (rc != EOK) {
        errno = rc;
        return -1;
    }

    uint32_t mode = 0;
    rc = ext4_mode_get(real_path, &mode);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }

    st->st_mode = (mode_t)mode;

    if ((st->st_mode & S_IFMT) == S_IFREG) {
        ext4_file f;
        rc = ext4_fopen(&f, real_path, "r");
        if (rc != EOK) {
            errno = rc;
            return -1;
        }
        st->st_size = (off_t)ext4_fsize(&f);
        ext4_fclose(&f);
    }

    return 0;
}

static int vfs_unlink(const char *path)
{
    char full[VFS_PATH_BUF_SZ];
    build_full_path(full, sizeof(full), path);
    int rc = ext4_fremove(full);
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_rename(const char *src, const char *dst)
{
    char full_src[VFS_PATH_BUF_SZ];
    char full_dst[VFS_PATH_BUF_SZ];
    build_full_path(full_src, sizeof(full_src), src);
    build_full_path(full_dst, sizeof(full_dst), dst);
    int rc = ext4_frename(full_src, full_dst);
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    char full[VFS_PATH_BUF_SZ];
    build_full_dir_path(full, sizeof(full), path);
    int rc = ext4_dir_mk(full);
    if (rc != EOK) { errno = rc; return -1; }
    return 0;
}

static int vfs_rmdir(const char *path)
{
    char full[VFS_PATH_BUF_SZ];
    /* ext4_dir_rm() computes name_off via ext4_generic_open(), which advances
     * past every '/' separator.  A trailing slash causes name_off to overshoot
     * the directory name, leaving path="/" and len=0 for the final ext4_unlink
     * call — resulting in ENOENT.  Use build_full_path (no trailing slash). */
    build_full_path(full, sizeof(full), path);
    ESP_LOGI(TAG, "ext4_dir_rm('%s')", full);
    int rc = ext4_dir_rm(full);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_dir_rm('%s') failed: rc=%d errno=%d", full, rc, errno);
        errno = rc;
        return -1;
    }
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
    char raw_path[VFS_PATH_BUF_SZ];
    char real_path[VFS_PATH_BUF_SZ];
    build_full_path(raw_path, sizeof(raw_path), path);

    int rc = resolve_symlink_path(raw_path, real_path, sizeof(real_path));
    if (rc != EOK) {
        errno = rc;
        return NULL;
    }

    size_t rlen = strlen(real_path);
    if (rlen > 0 && real_path[rlen - 1] != '/' && rlen < sizeof(real_path) - 1) {
        real_path[rlen] = '/';
        real_path[rlen + 1] = '\0';
    }

    rc = ext4_dir_open(&s_dirs[i].dir, real_path);
    if (rc != EOK) {
        ESP_LOGE(TAG, "ext4_dir_open('%s') failed: %d", real_path, rc);
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

    /* Skip deleted (inode=0) entries and the '.' / '..' pseudo-entries. */
    do {
        de = ext4_dir_entry_next(&d->dir);
        if (de == NULL) return NULL;
        if (de->inode == 0) continue;
        /* Skip '.' and '..' */
        if (de->name_length == 1 && de->name[0] == '.') continue;
        if (de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.') continue;
        break;
    } while (1);

    static struct dirent ent;
    memset(&ent, 0, sizeof(ent));
    uint8_t nlen = de->name_length < sizeof(ent.d_name) - 1
                 ? de->name_length : (uint8_t)(sizeof(ent.d_name) - 1);
    memcpy(ent.d_name, de->name, nlen);
    ent.d_name[nlen] = '\0';
    switch (de->inode_type) {
        case EXT4_DE_DIR:
            ent.d_type = DT_DIR;
            break;
        case EXT4_DE_SYMLINK:
            ent.d_type = DT_LNK;
            break;
        default:
            ent.d_type = DT_REG;
            break;
    }
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

int lwext4_rmdir_recursive(const char *path)
{
    /* ext4_dir_rm() must receive the path WITHOUT a trailing slash.
     * A trailing slash causes name_off to overshoot the directory name,
     * making the final ext4_unlink call use len=0 → ENOENT. */
    char full[VFS_PATH_BUF_SZ];
    size_t len = strnlen(path, sizeof(full) - 1);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    memcpy(full, path, len);
    full[len] = '\0';

    int rc = ext4_dir_rm(full);
    if (rc != EOK) {
        ESP_LOGE(TAG, "lwext4_rmdir_recursive('%s') failed: rc=%d", full, rc);
    }
    return rc;
}
