# sdmsg

`sdmsg`（“SD massage”）对块设备或普通文件做**原地重写**，给长期闲置、缓慢失电的闪存“马杀鸡”。基于 Meson 的 **C++** 程序，可选 **wxWidgets** 界面，并用 SQLite 记录管理数据。

## 用法

```bash
sdmsg [OPTIONS] DEVICE/FILE
```

| 选项 | 含义 |
|------|------|
| `-b/--block-size NUM` | I/O 块大小（默认自动探测） |
| `-d/--sqlite-db FILE` | 管理用 SQLite 数据库 |
| `-t/--test` | 对照数据库校验（不重写） |
| `-l/--linear` | 整盘/整文件裸重写（**默认**） |
| `-r/--recursive` | 按 FAT/exFAT、NTFS、ext2/3/4 盘上结构遍历重写 |
| `-g`, `--gui` | wxWidgets 进度/格子界面（需要显示器） |
| `-v/-q/-h/--version` | 日志 / 帮助 / 版本 |

默认无界面（进度打到 stderr）。需要窗口时加 `-g` / `--gui`。

重写时设备须**未挂载**，且只重写文件系统**已分配**数据（跳过空闲区；NTFS
压缩/加密按盘上字节原样重写）。**SHA-1** 在重写结束后、**挂载**状态下计算。
已有挂载会先卸下，结束后恢复到最初状态（含读写）。启用了数据库才计算
SHA-1（无界面时总会用数据库；图形界面要先打开数据库），并且图形界面不会在
终端里提问。普通文件和 loop 用 FUSE 只读挂载。真实磁盘用 udisks，然后
`sudo`/`pkexec`。编辑菜单里的 Dry-run 只读扫描，不写回。

有分区表时，linear 模式会分别重写分区表与各分区。recursive **不**用已挂载的
传统 FS 驱动做重写，而是自行解析盘上结构。

对普通文件操作时**不会截短**文件。

## 构建

```bash
sudo apt install meson ninja-build g++ pkg-config \
  libsqlite3-dev libssl-dev libwxgtk3.2-dev libbas-c-dev asciidoctor
meson setup /build
ninja -C /build
meson test -C /build
```

## 许可证

Copyright (C) 2026 Lenik <sdmsg@bodz.net>

采用 **AGPL-3.0-or-later** 许可。完整文本见 `LICENSE`。
