/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Synthetic image tests for linear rewrite, SQLite, FAT and ext scanners.
 */

#include "db/store.hpp"
#include "engine/massage.hpp"
#include "fs/detect.hpp"
#include "io/device.hpp"
#include "options.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures = 0;

#define CHECK(cond, msg)                                                                               \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            fprintf(stderr, "FAIL: %s\n", msg);                                                        \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

static std::string tmp_path(const char *pfx) {
    char buf[] = "/tmp/sdmsg-ut.XXXXXX";
    int fd = mkstemp(buf);
    if (fd >= 0)
        close(fd);
    unlink(buf);
    return std::string(buf) + pfx;
}

static int run_cmd(const std::string &cmd) {
    int rc = system(cmd.c_str());
    if (rc < 0)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

static void test_linear_no_truncate() {
    std::string img = tmp_path(".img");
    std::string db = tmp_path(".sqlite");
    {
        std::ofstream out(img, std::ios::binary);
        std::string payload(64 * 4096, '\x5a');
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    sdmsg::Options opts;
    opts.target = img;
    opts.sqlite_db = db;
    opts.block_size = 4096;
    opts.mode = sdmsg::RunMode::Linear;
    opts.dry_run = true;
    {
        struct stat st_dry {};
        CHECK(stat(img.c_str(), &st_dry) == 0, "stat before dry-run");
        sdmsg::MassageEngine dry(opts);
        CHECK(dry.run() == 0, "dry-run");
        struct stat st_dry_after {};
        CHECK(stat(img.c_str(), &st_dry_after) == 0, "stat after dry-run");
        CHECK(st_dry.st_mtim.tv_sec == st_dry_after.st_mtim.tv_sec &&
                  st_dry.st_mtim.tv_nsec == st_dry_after.st_mtim.tv_nsec,
              "dry-run left the file untouched");
    }
    opts.dry_run = false;

    struct stat st_before {};
    CHECK(stat(img.c_str(), &st_before) == 0, "stat before");
    sdmsg::MassageEngine eng(opts);
    int rc = eng.run();
    CHECK(rc == 0, "linear run");

    struct stat st_after {};
    CHECK(stat(img.c_str(), &st_after) == 0, "stat after");
    CHECK(st_before.st_size == st_after.st_size, "size unchanged");

    sdmsg::Store store;
    CHECK(store.open(db), "open db");
    sdmsg::ObjectRecord o;
    CHECK(store.get_object("::linear", o), "object ::linear");
    CHECK(o.massage_count >= 1, "massage_count");

    opts.action = sdmsg::Action::Test;
    sdmsg::MassageEngine eng2(opts);
    CHECK(eng2.run() == 0, "verify run");

    unlink(img.c_str());
    unlink(db.c_str());
}

static void test_fat_scan_and_rewrite() {
    if (run_cmd("command -v mkfs.vfat >/dev/null && command -v mcopy >/dev/null") != 0) {
        fprintf(stderr, "SKIP: fat test (mkfs.vfat/mcopy missing)\n");
        return;
    }
    std::string img = tmp_path(".fat");
    std::string db = tmp_path(".sqlite");
    CHECK(run_cmd("dd if=/dev/zero of='" + img + "' bs=1M count=8 status=none") == 0, "dd fat");
    CHECK(run_cmd("mkfs.vfat -F 32 '" + img + "' >/dev/null 2>&1") == 0, "mkfs fat");
    setenv("MTOOLS_SKIP_CHECK", "1", 1);
    CHECK(run_cmd("echo hi > /tmp/sdmsg-ut-f.txt && mcopy -i '" + img +
                  "' /tmp/sdmsg-ut-f.txt ::/f.txt") == 0,
          "mcopy");

    sdmsg::Device dev;
    CHECK(dev.open(img), "open fat image");
    auto ft = sdmsg::detect_fs(dev, 0);
    CHECK(ft == sdmsg::FsType::Fat32, "detect fat32");
    sdmsg::FsScanResult scan;
    CHECK(sdmsg::scan_filesystem(dev, 0, ft, scan), "scan fat");
    bool found = false;
    for (const auto &o : scan.objects) {
        if (o.path.find("F.TXT") != std::string::npos || o.path.find("f.txt") != std::string::npos)
            found = true;
    }
    CHECK(found, "found f.txt in scan");
    dev.close();

    sdmsg::Options opts;
    opts.target = img;
    opts.sqlite_db = db;
    opts.block_size = 512;
    opts.mode = sdmsg::RunMode::Recursive;
    sdmsg::MassageEngine eng(opts);
    CHECK(eng.run() == 0, "fat recursive massage");

    CHECK(run_cmd("mcopy -i '" + img + "' ::/f.txt /tmp/sdmsg-ut-f-out.txt >/dev/null") == 0,
          "mcopy out");
    std::ifstream in("/tmp/sdmsg-ut-f-out.txt");
    std::string line;
    std::getline(in, line);
    CHECK(line == "hi", "fat content preserved");

    unlink(img.c_str());
    unlink(db.c_str());
    unlink("/tmp/sdmsg-ut-f.txt");
    unlink("/tmp/sdmsg-ut-f-out.txt");
}

static void test_ext_scan() {
    if (run_cmd("command -v mkfs.ext4 >/dev/null && command -v debugfs >/dev/null") != 0) {
        fprintf(stderr, "SKIP: ext test (mkfs.ext4/debugfs missing)\n");
        return;
    }
    std::string img = tmp_path(".ext");
    std::string db = tmp_path(".sqlite");
    CHECK(run_cmd("dd if=/dev/zero of='" + img + "' bs=1M count=16 status=none") == 0, "dd ext");
    CHECK(run_cmd("mkfs.ext4 -F -q '" + img + "'") == 0, "mkfs ext4");
    CHECK(run_cmd("echo zzz > /tmp/sdmsg-ut-e.txt && debugfs -w -R 'write /tmp/sdmsg-ut-e.txt e.txt' '" +
                  img + "' >/dev/null 2>&1") == 0,
          "debugfs write");

    sdmsg::Device dev;
    CHECK(dev.open(img), "open ext");
    auto ft = sdmsg::detect_fs(dev, 0);
    CHECK(ft == sdmsg::FsType::Ext4 || ft == sdmsg::FsType::Ext3 || ft == sdmsg::FsType::Ext2,
          "detect ext");
    sdmsg::FsScanResult scan;
    CHECK(sdmsg::scan_filesystem(dev, 0, ft, scan), "scan ext");
    bool found = false;
    for (const auto &o : scan.objects) {
        if (o.path.find("e.txt") != std::string::npos)
            found = true;
    }
    CHECK(found, "found e.txt");
    dev.close();

    sdmsg::Options opts;
    opts.target = img;
    opts.sqlite_db = db;
    opts.block_size = 1024;
    opts.mode = sdmsg::RunMode::Recursive;
    sdmsg::MassageEngine eng(opts);
    CHECK(eng.run() == 0, "ext recursive");
    CHECK(run_cmd("debugfs -R 'cat e.txt' '" + img + "' 2>/dev/null | grep -q zzz") == 0,
          "ext content");

    unlink(img.c_str());
    unlink(db.c_str());
    unlink("/tmp/sdmsg-ut-e.txt");
}

int main() {
    test_linear_no_truncate();
    test_fat_scan_and_rewrite();
    test_ext_scan();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "all tests passed\n");
    return 0;
}
