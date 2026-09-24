/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "io/devices.hpp"

#include <cstdio>
#include <map>
#include <sstream>

namespace reflash {

namespace {

std::string run_capture(const char *cmd) {
    std::string out;
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return out;
    char buf[4096];
    while (fgets(buf, sizeof buf, fp))
        out += buf;
    pclose(fp);
    return out;
}

std::map<std::string, std::string> parse_lsblk_p_line(const std::string &line) {
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\n'))
            ++i;
        if (i >= line.size())
            break;
        size_t eq = line.find('=', i);
        if (eq == std::string::npos)
            break;
        std::string key = line.substr(i, eq - i);
        i = eq + 1;
        if (i >= line.size() || line[i] != '"')
            break;
        ++i;
        std::string val;
        while (i < line.size() && line[i] != '"') {
            if (line[i] == '\\' && i + 1 < line.size()) {
                val += line[i + 1];
                i += 2;
            } else {
                val += line[i++];
            }
        }
        if (i < line.size() && line[i] == '"')
            ++i;
        kv[key] = val;
    }
    return kv;
}

} /* namespace */

std::vector<BlockDevInfo> list_block_devices() {
    std::vector<BlockDevInfo> out;
    /* -P: KEY="value" lines; PKNAME = parent disk for partitions */
    std::string text = run_capture(
        "lsblk -P -o NAME,PATH,TYPE,SIZE,MODEL,TRAN,RM,MOUNTPOINT,PKNAME,LABEL,FSTYPE 2>/dev/null");
    if (text.empty())
        return out;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        auto kv = parse_lsblk_p_line(line);
        if (kv["PATH"].empty() || kv["NAME"].empty())
            continue;
        BlockDevInfo d;
        d.name = kv["NAME"];
        d.path = kv["PATH"];
        d.type = kv["TYPE"];
        d.size = kv["SIZE"];
        d.model = kv["MODEL"];
        d.transport = kv["TRAN"];
        d.mountpoint = kv["MOUNTPOINT"];
        d.label = kv["LABEL"];
        d.fstype = kv["FSTYPE"];
        d.parent = kv["PKNAME"];
        d.removable = (kv["RM"] == "1");
        out.push_back(std::move(d));
    }
    return out;
}

std::string device_group_label(const BlockDevInfo &d) {
    if (d.type == "loop")
        return "Loop";
    if (d.removable || d.transport == "usb")
        return "Removable";
    if (d.type == "disk" || d.type == "part")
        return "Disks";
    if (d.type == "rom")
        return "Optical";
    return "Other";
}

} /* namespace reflash */
