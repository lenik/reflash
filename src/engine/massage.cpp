/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "engine/massage.hpp"
#include "engine/sha1_pass.hpp"
#include "fs/detect.hpp"
#include "io/block_io.hpp"
#include "io/device.hpp"
#include "io/partition.hpp"
#include "mount/automount.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace sdmsg {

MassageEngine::MassageEngine(Options opts) : opts_(std::move(opts)) {}

MassageEngine::~MassageEngine() {
    pause_.request_stop();
    join();
}

std::uint64_t MassageEngine::resolve_block_size(Device &dev) const {
    if (opts_.block_size)
        return opts_.block_size;
    return dev.sector_size() ? dev.sector_size() : 4096;
}

bool MassageEngine::prepare_db(std::string *err) {
    std::string path = opts_.sqlite_db.empty() ? default_db_path(opts_.target) : opts_.sqlite_db;
    if (!store_.open(path, err))
        return false;
    store_.set_meta("target", opts_.target);
    store_.set_meta("mode", opts_.mode == RunMode::Linear ? "linear" : "recursive");
    return true;
}

static void record_rewrite(Store &store, const std::string &path, std::uint64_t size,
                           const BlockIoResult &r) {
    std::vector<ExtentRecord> exts;
    for (const auto &e : r.errors) {
        ExtentRecord er;
        er.path = path;
        er.offset = static_cast<std::int64_t>(e.offset);
        er.length = static_cast<std::int64_t>(e.length);
        er.status = e.status == ExtentStatus::ReadErr ? 1 : 2;
        exts.push_back(er);
    }
    store.replace_extents(path, exts);
    store.bump_massage(path, static_cast<std::int64_t>(size));
}

int MassageEngine::run_inner() {
    std::string err;
    std::vector<MountRecord> original;
    bool interactive = isatty(STDIN_FILENO) != 0;

    /* Rewrite always requires unmounted device/file-as-block. */
    if (!ensure_unmounted(opts_.target, opts_.auto_mount, &original, &err)) {
        fprintf(stderr, "sdmsg: %s\n", err.c_str());
        return 1;
    }

    Device dev;
    if (!dev.open(opts_.target)) {
        fprintf(stderr, "sdmsg: cannot open %s: %s\n", opts_.target.c_str(), strerror(errno));
        restore_mount_state(opts_.target, original, {}, false, nullptr);
        return 1;
    }

    if (!prepare_db(&err)) {
        fprintf(stderr, "sdmsg: database: %s\n", err.c_str());
        dev.close();
        restore_mount_state(opts_.target, original, {}, false, nullptr);
        return 1;
    }

    std::uint64_t bs = resolve_block_size(dev);
    store_.set_meta("block_size", std::to_string(bs));
    bool verify_only = opts_.action == Action::Test;

    /* -t: skip rewrite, go straight to mounted SHA-1 verify */
    int rc = 0;
    if (!verify_only) {
        auto run_one = [&](const std::string &path, std::uint64_t off, std::uint64_t len) -> bool {
            progress_.set_phase(path);
            auto r = rewrite_range(dev, off, len, bs, false, pause_, progress_, path);
            record_rewrite(store_, path, len, r);
            return true;
        };

        if (opts_.mode == RunMode::Linear) {
            PartitionMap pm;
            if (!probe_partitions(dev, pm, &err)) {
                fprintf(stderr, "sdmsg: partition probe: %s\n", err.c_str());
                rc = 1;
            } else if (pm.kind != PartitionTableKind::None && !pm.parts.empty()) {
                std::uint64_t total = pm.table_bytes;
                for (const auto &p : pm.parts)
                    total += p.length_bytes;
                progress_.reset(total, bs);
                store_.set_meta("cell_shift", std::to_string(progress_.cell_shift()));

                std::string table_name =
                    pm.kind == PartitionTableKind::Gpt ? "::gpt" : "::mbr";
                run_one(table_name, 0, pm.table_bytes);
                for (const auto &p : pm.parts) {
                    if (pause_.stop_requested())
                        break;
                    run_one(p.name, p.start_bytes, p.length_bytes);
                }
            } else {
                progress_.reset(dev.size(), bs);
                store_.set_meta("cell_shift", std::to_string(progress_.cell_shift()));
                run_one("::linear", 0, dev.size());
            }
        } else {
            PartitionMap pm;
            probe_partitions(dev, pm, nullptr);

            struct Job {
                std::string label;
                std::uint64_t base;
            };
            std::vector<Job> jobs;
            if (pm.kind != PartitionTableKind::None && !pm.parts.empty()) {
                for (const auto &p : pm.parts)
                    jobs.push_back({p.name, p.start_bytes});
            } else {
                jobs.push_back({"::fs", 0});
            }

            struct WorkItem {
                std::string path;
                std::uint64_t offset;
                std::uint64_t length;
            };
            std::vector<WorkItem> work;
            std::uint64_t total = 0;

            {
                std::string ignore;
                if (detect_fs(dev, 0, &ignore) != FsType::Unknown && pm.parts.empty()) {
                    pm.kind = PartitionTableKind::None;
                    pm.table_bytes = 0;
                    jobs.clear();
                    jobs.push_back({"::fs", 0});
                }
            }

            if (pm.kind != PartitionTableKind::None && pm.table_bytes > 0 && !pm.parts.empty()) {
                std::string table_name =
                    pm.kind == PartitionTableKind::Gpt ? "::gpt" : "::mbr";
                work.push_back({table_name, 0, pm.table_bytes});
                total += pm.table_bytes;
            }

            for (const auto &job : jobs) {
                std::string derr;
                FsType ft = detect_fs(dev, job.base, &derr);
                if (ft == FsType::Unknown) {
                    fprintf(stderr, "sdmsg: %s: %s — rewriting partition linearly\n",
                            job.label.c_str(), derr.c_str());
                    std::uint64_t len = 0;
                    for (const auto &p : pm.parts) {
                        if (p.start_bytes == job.base) {
                            len = p.length_bytes;
                            break;
                        }
                    }
                    if (len == 0)
                        len = dev.size() > job.base ? dev.size() - job.base : 0;
                    work.push_back({job.label, job.base, len});
                    total += len;
                    continue;
                }
                FsScanResult scan;
                if (!scan_filesystem(dev, job.base, ft, scan)) {
                    fprintf(stderr, "sdmsg: scan %s (%s): %s\n", job.label.c_str(),
                            fs_type_name(ft), scan.error.c_str());
                    rc = 1;
                    continue;
                }
                fprintf(stderr, "sdmsg: %s: detected %s, %zu objects\n", job.label.c_str(),
                        fs_type_name(ft), scan.objects.size());
                for (const auto &obj : scan.objects) {
                    ObjectRecord rec;
                    rec.path = obj.path;
                    rec.size = static_cast<std::int64_t>(obj.size);
                    store_.upsert_object(rec);
                    for (const auto &e : obj.extents) {
                        if (e.length == 0)
                            continue;
                        work.push_back({obj.path, e.offset, e.length});
                        total += e.length;
                    }
                }
            }

            progress_.reset(total ? total : 1, bs);
            store_.set_meta("cell_shift", std::to_string(progress_.cell_shift()));

            for (const auto &w : work) {
                if (pause_.stop_requested())
                    break;
                run_one(w.path, w.offset, w.length);
            }
        }

        if (pause_.stop_requested())
            rc = 1;
    }

    /* Close block device before mounting. */
    dev.close();

    auto bad = store_.bad_extents();
    if (!bad.empty()) {
        fprintf(stderr, "sdmsg: %zu bad extent(s):\n", bad.size());
        for (const auto &e : bad) {
            fprintf(stderr, "  %s offset=%lld length=%lld status=%d\n", e.path.c_str(),
                    static_cast<long long>(e.offset), static_cast<long long>(e.length), e.status);
        }
        fprintf(stderr,
                "sdmsg: export this list for badblocks / e2fsck -l (filesystem-specific).\n");
    }

    /* SHA-1 phase: requires mount. */
    std::vector<MountRecord> active;
    bool temp_created = false;
    bool do_sha1 = true;
    if (!prepare_sha1_mounts(opts_.target, opts_.auto_mount, interactive, original, &active,
                             &temp_created, &err)) {
        if (!err.empty())
            fprintf(stderr, "sdmsg: SHA-1 mount: %s\n", err.c_str());
        else
            fprintf(stderr, "sdmsg: skipping SHA-1\n");
        do_sha1 = false;
    }

    if (do_sha1 && !active.empty()) {
        progress_.set_phase("sha1");
        std::string serr;
        if (!sha1_mounted_files(store_, active, progress_, verify_only, &serr)) {
            fprintf(stderr, "sdmsg: SHA-1: %s\n", serr.c_str());
            rc = 1;
        }
    }

    {
        std::string merr;
        if (!restore_mount_state(opts_.target, original, active, temp_created, &merr))
            fprintf(stderr, "sdmsg: restore mount: %s\n", merr.c_str());
    }

    progress_.set_finished(rc != 0);
    return rc;
}

int MassageEngine::run() {
    running_ = true;
    exit_code_ = run_inner();
    running_ = false;
    return exit_code_;
}

bool MassageEngine::start(std::string *err) {
    if (running_) {
        if (err)
            *err = "already running";
        return false;
    }
    running_ = true;
    worker_ = std::thread([this] {
        exit_code_ = run_inner();
        running_ = false;
    });
    return true;
}

void MassageEngine::join() {
    if (worker_.joinable())
        worker_.join();
}

} /* namespace sdmsg */
