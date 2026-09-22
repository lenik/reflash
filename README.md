# sdmsg

`sdmsg` (“SD massage”) rewrites block devices or regular files **in place** to
refresh flash storage that sits unused and slowly loses charge.  It is a
Meson-built **C++** tool with an optional **wxWidgets** UI and a SQLite
management database.

## Usage

```bash
sdmsg [OPTIONS] DEVICE/FILE
```

| Option | Meaning |
|--------|---------|
| `-b/--block-size NUM` | I/O size (default: auto) |
| `-d/--sqlite-db FILE` | Management database |
| `-t/--test` | Verify against DB (no rewrite) |
| `-l/--linear` | Raw whole-device/file rewrite (**default**) |
| `-r/--recursive` | On-disk FAT/exFAT, NTFS, ext2/3/4 walk (must be unmounted) |
| `-m/--auto-mount` | Unmount before run; remount after |
| `--gui` | wxWidgets progress/grid UI (needs a display) |
| `-v/-q/-h/--version` | Logging / help / version |

Headless by default (progress on stderr).  Use `--gui` for the window.

Rewrite runs **unmounted** and only rewrites **allocated** filesystem data
(free space skipped; NTFS compression/encryption rewritten as on-disk bytes).
**SHA-1** is computed afterward on a **mounted** filesystem.  With `-m`, mounts
are restored to the original RW/RO state.  Without `-m`, you can skip SHA-1 or
mount read-only (`sudo`/`pkexec`); set `SDMSG_SHA1=skip|mount` for non-interactive use.

Linear mode rewrites the partition table and each partition when a table is
present.  Recursive mode does **not** use mounted VFS drivers for rewrite; it
parses filesystem structures directly.

Regular files are never truncated.

## Build

```bash
sudo apt install meson ninja-build g++ pkg-config \
  libsqlite3-dev libssl-dev libwxgtk3.2-dev libbas-c-dev asciidoctor
meson setup /build
ninja -C /build
meson test -C /build
```

## License

Copyright (C) 2026 Lenik <sdmsg@bodz.net>

Licensed under **AGPL-3.0-or-later**.  
See `LICENSE` for the full text and supplemental project terms.
