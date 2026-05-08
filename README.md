# emmc-fs

FAT and EXT4 filesystem abstraction for ESP32 eMMC/SD cards via ESP-IDF.

[![Crates.io](https://img.shields.io/crates/v/emmc-fs.svg)](https://crates.io/crates/emmc-fs)
[![Documentation](https://docs.rs/emmc-fs/badge.svg)](https://docs.rs/emmc-fs)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## Features

- **FAT support**: FAT12/16/32 via `esp_vfs_fat_sdmmc_mount` (enabled by default)
- **EXT4 support**: EXT2/3/4 via the `lwext4_blockdev` ESP-IDF component (optional feature)
- **Auto-detect**: Scan the MBR to detect the filesystem type on the card
- **VFS integration**: Mounts register the filesystem at a VFS path so that standard `std::fs` operations work transparently
- **Auto-unmount**: `FatMount` and `Ext4Mount` unmount automatically when dropped

| Feature | Description |
|---------|-------------|
| `fat`   | FAT12/16/32 support via `esp_vfs_fat` (enabled by default) |
| `ext4`  | EXT2/3/4 support via the `lwext4_blockdev` ESP-IDF component |

## Quick Start

Add this to your `Cargo.toml`:

```toml
[dependencies]
emmc-fs = "0.1.0"
```

For EXT4 support, enable the `ext4` feature:

```toml
[dependencies]
emmc-fs = { version = "0.1.0", features = ["ext4"] }
```

## EXT4 Prerequisites

Before building with the `ext4` feature, clone the lwext4 source into the component directory:

```sh
cd emmc-fs/components/lwext4_blockdev
git clone https://github.com/gkostka/lwext4 lwext4
```

Also add the component to your top-level `Cargo.toml`:

```toml
[[package.metadata.esp-idf-sys.extra_components]]
component_dirs = ["emmc-fs/components/lwext4_blockdev"]
```

## Example Usage

### FAT

```rust
use emmc_fs::fat::FatMount;
use esp_idf_sys::{sdmmc_host_t, sdmmc_slot_config_t, esp_vfs_fat_sdmmc_mount_config_t};

let mount = unsafe {
    FatMount::mount(
        &host,
        &slot_config,
        b"/eMMC\0",
        &mount_config,
    )?
};

// Standard std::fs operations now work on /eMMC/...
std::fs::write("/eMMC/hello.txt", b"Hello, eMMC!")?;
let data = std::fs::read("/eMMC/hello.txt")?;

// FatMount unmounts automatically when dropped
```

### EXT4

```rust
#[cfg(feature = "ext4")]
use emmc_fs::ext4::Ext4Mount;

#[cfg(feature = "ext4")]
let mount = unsafe {
    Ext4Mount::mount(&host, &slot_config, "/ext4")?
};

// Standard std::fs operations now work on /ext4/...
std::fs::write("/ext4/hello.txt", b"Hello, EXT4!")?;

// Ext4Mount unmounts automatically when dropped
```

### Filesystem Auto-detection

```rust
use emmc_fs::{detect, scan_mbr, FsType};

// Scan MBR partition table
let partitions = unsafe { scan_mbr(card) };
for p in &partitions {
    println!("Partition type=0x{:02X} lba_start={} sectors={}", 
        p.part_type, p.lba_start, p.sector_count);
    if p.is_fat()  { println!("  -> FAT"); }
    if p.is_ext()  { println!("  -> EXT4"); }
}

// Or detect the primary filesystem type directly
let fs_type = unsafe { detect(card) };
match fs_type {
    FsType::Fat  => println!("FAT filesystem detected"),
    FsType::Ext4 => println!("EXT4 filesystem detected"),
    FsType::Unknown => println!("Unknown filesystem"),
}
```

## Platform

This crate targets the ESP32 family running ESP-IDF and depends on:

- [`esp-idf-sys`](https://crates.io/crates/esp-idf-sys) for ESP-IDF bindings
- [`embuild`](https://crates.io/crates/embuild) for build-time ESP-IDF integration

It is designed to be used with an ESP32-S3 (or compatible) board connected to an eMMC or SD card via the SDMMC host peripheral.

## License

This crate (`emmc-fs`) is licensed under the **MIT License** — see [LICENSE](LICENSE).

### Third-party components

The `ext4` feature depends on [lwext4](https://github.com/gkostka/lwext4), which is licensed under the **GNU General Public License v2 (GPL-2.0)**. The lwext4 source is located under `components/lwext4_blockdev/lwext4/` and is **not** covered by this crate's MIT License.

> **Note**: If you distribute a binary built with the `ext4` feature enabled, the GPL-2.0 terms of lwext4 apply and you must comply with its requirements (source disclosure, etc.). Builds using only the `fat` feature are unaffected.
