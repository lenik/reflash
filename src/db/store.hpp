/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace sdmsg {

enum class VerifyStatus : int { Unknown = 0, Ok = 1, Fail = 2 };

struct ObjectRecord {
    std::string path;
    std::int64_t size = 0;
    unsigned char sha1[20]{};
    bool has_sha1 = false;
    std::int64_t last_massage = 0;
    std::int64_t massage_count = 0;
    VerifyStatus verify_status = VerifyStatus::Unknown;
    std::int64_t verify_count = 0;
};

struct ExtentRecord {
    std::string path;
    std::int64_t offset = 0;
    std::int64_t length = 0;
    int status = 0; /* 0 ok, 1 read_err, 2 write_err */
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

private:
    sqlite3 *db_ = nullptr;
    bool exec(const char *sql, std::string *err);
    bool migrate(std::string *err);
};

std::string default_db_path(const std::string &target);

} /* namespace sdmsg */
