/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace sdmsg {

enum class VerifyStatus : int { Unknown = 0, Ok = 1, Fail = 2 };

/* Engine-facing record. Special paths start with "::" (partition/FS objects).
 * Other paths are files under the current volume. */
struct ObjectRecord {
    std::string path;
    std::int64_t size = 0;
    unsigned char sha1[20]{};
    bool has_sha1 = false;
    std::int64_t last_massage = 0;
    std::int64_t massage_count = 0;
    VerifyStatus verify_status = VerifyStatus::Unknown;
    std::int64_t verify_count = 0;
    std::int64_t verified_at = 0;
};

struct ExtentRecord {
    std::string path;
    std::int64_t offset = 0;
    std::int64_t length = 0;
    int status = 0; /* 0 ok, 1 read_err, 2 write_err */
};

/* One child of a folder, as stored in the catalog (no filesystem view). */
struct DirDbEntry {
    std::string basename;
    bool directory = false;
    std::int64_t id = 0; /* folder id or file id */
    std::int64_t size = 0;
    bool has_sha1 = false;
    unsigned char sha1[20]{};
    VerifyStatus verify_status = VerifyStatus::Unknown;
    std::int64_t verified_at = 0;
};

class Store {
public:
    Store() = default;
    ~Store();

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    bool open(const std::string &path, std::string *err = nullptr);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string &path() const { return path_; }

    bool check_integrity(std::string *report, std::string *err = nullptr);

    /*
     * Multi-phase catalog validation with progress (counts, cross-refs,
     * PRAGMA quick_check / integrity_check). Honour pause/cancel between
     * batches; cancel also interrupts SQLite via sqlite3_interrupt.
     */
    struct ValidateStatus {
        std::string phase;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        double eta_seconds = -1.0;
        bool finished = false;
        bool failed = false;
        bool cancelled = false;
    };
    using ValidateProgressFn = std::function<void(const ValidateStatus &)>;
    bool validate_manifest(ValidateProgressFn on_progress, std::atomic<bool> *pause,
                           std::atomic<bool> *cancel, std::string *report,
                           std::string *err = nullptr);

    bool upsert_object(const ObjectRecord &o, std::string *err = nullptr);
    bool get_object(const std::string &path, ObjectRecord &out, std::string *err = nullptr);
    bool bump_massage(const std::string &path, std::int64_t size, std::string *err = nullptr);
    bool set_sha1(const std::string &path, const unsigned char sha1[20], std::string *err = nullptr);
    bool bump_verify(const std::string &path, VerifyStatus st, const unsigned char *sha1,
                     std::string *err = nullptr);
    std::vector<ObjectRecord> list_objects(std::string *err = nullptr);

    bool replace_extents(const std::string &path, const std::vector<ExtentRecord> &exts,
                         std::string *err = nullptr);
    bool set_meta(const std::string &key, const std::string &value, std::string *err = nullptr);
    bool get_meta(const std::string &key, std::string &value, std::string *err = nullptr);

    std::vector<ExtentRecord> bad_extents(std::string *err = nullptr);

    /* Days a stored SHA-1 may be reused without re-hashing. Default 183 (~6 months). */
    int sha1_valid_days();
    bool set_sha1_valid_days(int days, std::string *err = nullptr);

    std::int64_t current_volume_id();
    /* folder_id 0 is the volume root. */
    std::vector<DirDbEntry> list_db_dir(std::int64_t folder_id, std::string *err = nullptr);
    /* Number of file rows in the catalog (manifest records). */
    std::int64_t file_record_count(std::string *err = nullptr);
    std::int64_t ensure_folder(std::int64_t parent_id, const std::string &basename,
                               std::string *err = nullptr);
    /* Remove a catalog file path (and its extents). */
    bool delete_file(const std::string &path, std::string *err = nullptr);
    /* Remove an empty catalog folder (and nested empty folders / files under it). */
    bool delete_folder(std::int64_t folder_id, std::string *err = nullptr);

private:
    sqlite3 *db_ = nullptr;
    std::string path_;
    mutable std::recursive_mutex mu_;
    bool exec(const char *sql, std::string *err);
    bool migrate(std::string *err);
    bool table_exists(const char *name);
    std::int64_t ensure_volume_locked();
    std::int64_t ensure_root_folder_locked(std::int64_t vol);
    std::int64_t ensure_folder_locked(std::int64_t vol, std::int64_t parent,
                                      const std::string &basename, std::string *err);
    bool upsert_special_locked(const ObjectRecord &o, std::string *err);
    bool upsert_file_locked(const ObjectRecord &o, std::string *err);
    bool get_special_locked(const std::string &path, ObjectRecord &out);
    bool get_file_locked(const std::string &path, ObjectRecord &out);
    void import_legacy_objects();
};

std::string default_db_path(const std::string &target);

} /* namespace sdmsg */
