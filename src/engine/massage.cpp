/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "engine/massage.hpp"
#include "engine/sha1_pass.hpp"
#include "fs/detect.hpp"
#include "io/block_io.hpp"
#include "io/device.hpp"
#include "io/page_cache.hpp"
#include "io/partition.hpp"
#include "mount/automount.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <unistd.h>

namespace reflash {

namespace {

std::string format_mib(std::uint64_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

void log_rewrite_stats(Progress &progress, bool dry_run) {
    double sec = progress.elapsed_sec();
    if (sec < 0.001)
        sec = 0.001;
    std::uint64_t done = progress.bytes_done();
    double mibs = (static_cast<double>(done) / (1024.0 * 1024.0)) / sec;
    size_t ok = progress.count_cells(CellStatus::Ok);
    size_t cached = progress.count_cells(CellStatus::Cached);
    size_t rd = progress.count_cells(CellStatus::ReadErr);
    size_t wr = progress.count_cells(CellStatus::WriteErr);
    char line[256];
    std::snprintf(line, sizeof line,
                  "%s done: %s MiB in %.1fs (%.1f MiB/s); cells ok=%zu cached=%zu "
                  "read_err=%zu write_err=%zu",
                  dry_run ? "Dry-run" : "Rewrite", format_mib(done).c_str(), sec, mibs, ok, cached,
                  rd, wr);
    progress.message(LogLevel::Info, line);
}

} /* namespace */

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
    std::string path = opts_.sqlite_db;
    if (path.empty() && !opts_.sqlite_db_explicit)
        path = find_builtin_manifest_db(opts_.target);
    if (path.empty())
        path = default_db_path(opts_.target);
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

static void publish_fail(Progress &progress, const std::string &msg) {
    fprintf(stderr, "reflash: %s\n", msg.c_str());
    progress.reset(1, 4096);
    progress.log(0, 4096, CellStatus::ReadErr, msg);
    progress.set_phase("failed");
}

int MassageEngine::run_inner() {
    int rc = 1;
    struct FinishGuard {
        Progress &progress;
        int &rc;
        ~FinishGuard() { progress.set_finished(rc != 0); }
    } finish{progress_, rc};

    std::string err;
    std::vector<MountRecord> original;

    progress_.set_phase("prepare");
    progress_.message(LogLevel::Info, opts_.dry_run ? "Dry-run (read only)" : "Massage");

    /* Rewrite always requires unmounted device/file-as-block. */
    if (!ensure_unmounted(opts_.target, &original, &err)) {
        publish_fail(progress_, err.empty() ? "unmount failed" : err);
        return rc;
    }

    Device dev;
    if (!dev.open(opts_.target)) {
        publish_fail(progress_, std::string("cannot open ") + opts_.target + ": " + strerror(errno));
        restore_mount_state(opts_.target, original, {}, false, nullptr);
        return rc;
    }

    if (!prepare_db(&err)) {
        publish_fail(progress_, err.empty() ? "database open failed" : err);
        dev.close();
        restore_mount_state(opts_.target, original, {}, false, nullptr);
        return rc;
    }

    std::uint64_t bs = resolve_block_size(dev);
    store_.set_meta("block_size", std::to_string(bs));
    /* Show a grid immediately; the real map replaces this after the scan. */
    progress_.reset(dev.size() ? dev.size() : 1, bs);
    progress_.set_phase("prepare");
    bool verify_only = opts_.action == Action::Test;
    /* Database on: GUI only after the user opens one; headless always has a catalog. */
    bool database_on = !opts_.gui || !opts_.sqlite_db.empty();

    /* -t: skip rewrite, go straight to mounted SHA-1 verify.
     * Dry-run still walks, but rewrite_range only reads. */
    rc = 0;
    if (!verify_only) {
        auto run_one = [&](const std::string &path, std::uint64_t off, std::uint64_t len) -> bool {
            progress_.set_phase(path);
            RewriteFlags rf;
            rf.verify_only = opts_.dry_run;
            rf.write_cache = opts_.write_cache;
            rf.verify_writes = opts_.verify_writes;
            rf.flush_poll_ms = opts_.flush_poll_ms;
            auto r = rewrite_range(dev, off, len, bs, rf, pause_, progress_, path);
            if (!opts_.dry_run || !r.errors.empty())
                record_rewrite(store_, path, len, r);
            return true;
        };

        if (opts_.mode == RunMode::Linear) {
            PartitionMap pm;
            if (!probe_partitions(dev, pm, &err)) {
                progress_.message(LogLevel::Error, "partition probe: " + err);
                rc = 1;
            } else if (pm.kind != PartitionTableKind::None && !pm.parts.empty()) {
                std::uint64_t total = pm.table_bytes;
                for (const auto &p : pm.parts)
                    total += p.length_bytes;
                progress_.reset(total, bs);
                progress_.set_phase("prepare");
                progress_.message(LogLevel::Info, opts_.dry_run ? "Dry-run scan" : "Rewrite");
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
                progress_.set_phase("prepare");
                progress_.message(LogLevel::Info, opts_.dry_run ? "Dry-run scan" : "Rewrite");
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
                    progress_.message(LogLevel::Warn, job.label + ": " + derr +
                                                          " - rewriting partition linearly");
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
                    progress_.message(LogLevel::Error,
                                      std::string("scan ") + job.label + " (" + fs_type_name(ft) +
                                          "): " + scan.error);
                    rc = 1;
                    continue;
                }
                progress_.message(LogLevel::Info, job.label + ": detected " + fs_type_name(ft) +
                                                      ", " + std::to_string(scan.objects.size()) +
                                                      " objects");
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
            progress_.set_phase("prepare");
            progress_.message(LogLevel::Info, opts_.dry_run ? "Dry-run scan" : "Rewrite");
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

    if (!opts_.dry_run && !pause_.stop_requested() && !opts_.write_through()) {
        /*
         * Full-disk sync on a dedicated thread so this worker keeps waking to
         * update progress. GUI locks input (wait cursor; Pause/Stop disabled).
         */
        progress_.set_phase("flush");
        progress_.set_input_locked(true);
        progress_.message(LogLevel::Info, "Full-disk sync (background)…");
        const int poll_ms = opts_.flush_poll_ms > 0 ? opts_.flush_poll_ms : 780;
        const std::uint64_t sz = dev.size() ? dev.size() : 1;
        std::atomic<bool> sync_done{false};
        const auto sync_t0 = std::chrono::steady_clock::now();
        std::thread sync_thr([&]() {
            wait_flushed(dev.fd(), 0, sz, nullptr);
            ::fdatasync(dev.fd());
            sync_done.store(true, std::memory_order_release);
        });

        while (!sync_done.load(std::memory_order_acquire) && !pause_.stop_requested()) {
            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - sync_t0)
                             .count();
            char line[96];
            std::snprintf(line, sizeof line, "Full-disk sync… %.0fs", sec);
            progress_.set_phase(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        }
        if (sync_thr.joinable())
            sync_thr.join();
        progress_.set_input_locked(false);
        if (!pause_.stop_requested()) {
            progress_.upgrade_cached_to_ok();
            progress_.set_phase("flush");
            progress_.message(LogLevel::Info, "Full-disk sync done");
        }
    }
    log_rewrite_stats(progress_, opts_.dry_run);

    /* Close block device before mounting. */
    dev.close();

    auto bad = store_.bad_extents();
    if (!bad.empty()) {
        progress_.message(LogLevel::Warn, std::to_string(bad.size()) + " bad extent(s)");
        for (const auto &e : bad) {
            progress_.message(LogLevel::Error, e.path + " offset=" + std::to_string(e.offset) +
                                                   " length=" + std::to_string(e.length));
        }
    }

    /* SHA-1 only when a database is enabled. Never ask on the terminal. */
    std::vector<MountRecord> active;
    bool temp_created = false;
    bool do_sha1 = database_on && !pause_.stop_requested();
    if (!database_on) {
        progress_.message(LogLevel::Info, "SHA-1 skipped: no database");
    } else if (!prepare_sha1_mounts(opts_.target, original, &active, &temp_created, &err)) {
        progress_.message(LogLevel::Warn, err.empty() ? "SHA-1 mount failed" : "SHA-1 mount: " + err);
        do_sha1 = false;
    }

    if (do_sha1 && !active.empty()) {
        progress_.set_phase("sha1");
        std::string serr;
        progress_.message(LogLevel::Info, "SHA-1");
        if (!sha1_mounted_files(store_, active, progress_, verify_only, &serr)) {
            progress_.message(LogLevel::Error, serr.empty() ? "SHA-1 failed" : serr);
            rc = 1;
        }
    }

    {
        std::string merr;
        if (!restore_mount_state(opts_.target, original, active, temp_created, &merr))
            progress_.message(LogLevel::Warn, "restore mount: " + merr);
    }

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
    if (opts_.target.empty()) {
        if (err)
            *err = "no DEVICE/FILE selected";
        return false;
    }
    join();
    pause_.reset();
    progress_.note_restart();
    exit_code_ = 1;
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

} /* namespace reflash */
