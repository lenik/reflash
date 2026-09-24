/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <string>
#include <vector>

namespace reflash {

struct BlockDevInfo {
    std::string path;      /* /dev/sda, /dev/sda1 */
    std::string name;      /* sda, sda1 */
    std::string type;      /* disk, part, loop, rom, … */
    std::string size;      /* human, e.g. 480G */
    std::string model;
    std::string transport; /* nvme, usb, sata, … */
    std::string label;
    std::string fstype;
    std::string mountpoint;
    bool removable = false;
    std::string parent;    /* disk name for partitions, empty for disks */
};

/* Enumerate block devices via lsblk -J (best-effort). */
std::vector<BlockDevInfo> list_block_devices();

/* Group label for Device submenu: Disks / Removable / Loop / Other */
std::string device_group_label(const BlockDevInfo &d);

} /* namespace reflash */
