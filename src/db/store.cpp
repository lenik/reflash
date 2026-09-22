/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "db/store.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

namespace sdmsg {

Store::~Store() { close(); }

void Store::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Store::exec(const char *sql, std::string *err) {
    char *errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        if (err)
            *err = errmsg ? errmsg : "sqlite error";
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

bool Store::migrate(std::string *err) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS objects ("
        "  path TEXT PRIMARY KEY,"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  sha1 BLOB,"
        "  last_massage INTEGER NOT NULL DEFAULT 0,"
        "  massage_count INTEGER NOT NULL DEFAULT 0,"
        "  verify_status INTEGER NOT NULL DEFAULT 0,"
        "  verify_count INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS extents ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  status INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_extents_path ON extents(path);"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT"
        ");";
    return exec(ddl, err);
}

bool Store::open(const std::string &path, std::string *err) {
    close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_ ? db_ : nullptr);
        close();
        return false;
    }
    if (!migrate(err)) {
        close();
        return false;
    }
    return true;
}

bool Store::upsert_object(const ObjectRecord &o, std::string *err) {
    const char *sql =
        "INSERT INTO objects(path,size,sha1,last_massage,massage_count,verify_status,verify_count)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET"
        " size=excluded.size,"
        " sha1=COALESCE(excluded.sha1, objects.sha1),"
        " last_massage=excluded.last_massage,"
        " massage_count=excluded.massage_count,"
        " verify_status=excluded.verify_status,"
        " verify_count=excluded.verify_count;";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, o.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, o.size);
    if (o.has_sha1)
        sqlite3_bind_blob(st, 3, o.sha1, 20, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 3);
    sqlite3_bind_int64(st, 4, o.last_massage);
    sqlite3_bind_int64(st, 5, o.massage_count);
    sqlite3_bind_int(st, 6, static_cast<int>(o.verify_status));
    sqlite3_bind_int64(st, 7, o.verify_count);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::get_object(const std::string &path, ObjectRecord &out, std::string *err) {
    const char *sql =
        "SELECT path,size,sha1,last_massage,massage_count,verify_status,verify_count"
        " FROM objects WHERE path=?";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out.path = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        out.size = sqlite3_column_int64(st, 1);
        const void *blob = sqlite3_column_blob(st, 2);
        int blen = sqlite3_column_bytes(st, 2);
        out.has_sha1 = blob && blen == 20;
        if (out.has_sha1)
            std::memcpy(out.sha1, blob, 20);
        out.last_massage = sqlite3_column_int64(st, 3);
        out.massage_count = sqlite3_column_int64(st, 4);
        out.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
        out.verify_count = sqlite3_column_int64(st, 6);
        sqlite3_finalize(st);
        return true;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && err)
        *err = sqlite3_errmsg(db_);
    return false;
}

bool Store::bump_massage(const std::string &path, std::int64_t size, std::string *err) {
    ObjectRecord o;
    if (!get_object(path, o, nullptr)) {
        o.path = path;
        o.massage_count = 0;
    }
    o.size = size;
    /* SHA-1 is filled later while mounted — do not clear an existing digest here. */
    o.last_massage = static_cast<std::int64_t>(std::time(nullptr));
    o.massage_count += 1;
    return upsert_object(o, err);
}

bool Store::set_sha1(const std::string &path, const unsigned char sha1[20], std::string *err) {
    ObjectRecord o;
    if (!get_object(path, o, nullptr)) {
        o.path = path;
    }
    std::memcpy(o.sha1, sha1, 20);
    o.has_sha1 = true;
    return upsert_object(o, err);
}

bool Store::bump_verify(const std::string &path, VerifyStatus st, const unsigned char *sha1,
                        std::string *err) {
    ObjectRecord o;
    if (!get_object(path, o, nullptr)) {
        o.path = path;
    }
    if (sha1) {
        std::memcpy(o.sha1, sha1, 20);
        o.has_sha1 = true;
    }
    o.verify_status = st;
    o.verify_count += 1;
    return upsert_object(o, err);
}

std::vector<ObjectRecord> Store::list_objects(std::string *err) {
    std::vector<ObjectRecord> out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT path,size,sha1,last_massage,massage_count,verify_status,verify_count"
                           " FROM objects ORDER BY path",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        ObjectRecord o;
        o.path = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        o.size = sqlite3_column_int64(st, 1);
        const void *blob = sqlite3_column_blob(st, 2);
        int blen = sqlite3_column_bytes(st, 2);
        o.has_sha1 = blob && blen == 20;
        if (o.has_sha1)
            std::memcpy(o.sha1, blob, 20);
        o.last_massage = sqlite3_column_int64(st, 3);
        o.massage_count = sqlite3_column_int64(st, 4);
        o.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
        o.verify_count = sqlite3_column_int64(st, 6);
        out.push_back(o);
    }
    sqlite3_finalize(st);
    return out;
}

bool Store::replace_extents(const std::string &path, const std::vector<ExtentRecord> &exts,
                            std::string *err) {
    sqlite3_stmt *del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM extents WHERE path=?", -1, &del, nullptr) !=
        SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(del, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(del) != SQLITE_DONE) {
        if (err)
            *err = sqlite3_errmsg(db_);
        sqlite3_finalize(del);
        return false;
    }
    sqlite3_finalize(del);

    const char *ins =
        "INSERT INTO extents(path,offset,length,status,updated_at) VALUES(?,?,?,?,?)";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto &e : exts) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, e.offset);
        sqlite3_bind_int64(st, 3, e.length);
        sqlite3_bind_int(st, 4, e.status);
        sqlite3_bind_int64(st, 5, now);
        if (sqlite3_step(st) != SQLITE_DONE) {
            if (err)
                *err = sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            return false;
        }
    }
    sqlite3_finalize(st);
    return true;
}

bool Store::set_meta(const std::string &key, const std::string &value, std::string *err) {
    const char *sql = "INSERT INTO meta(key,value) VALUES(?,?)"
                      " ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::get_meta(const std::string &key, std::string &value, std::string *err) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key=?", -1, &st, nullptr) !=
        SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *v = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        value = v ? v : "";
        sqlite3_finalize(st);
        return true;
    }
    sqlite3_finalize(st);
    return false;
}

std::vector<ExtentRecord> Store::bad_extents(std::string *err) {
    std::vector<ExtentRecord> out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT path,offset,length,status FROM extents WHERE status!=0"
                           " ORDER BY offset",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        ExtentRecord e;
        e.path = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        e.offset = sqlite3_column_int64(st, 1);
        e.length = sqlite3_column_int64(st, 2);
        e.status = sqlite3_column_int(st, 3);
        out.push_back(e);
    }
    sqlite3_finalize(st);
    return out;
}

std::string default_db_path(const std::string &target) {
    const char *home = getenv("HOME");
    std::string cache = home ? std::string(home) + "/.cache/sdmsg" : "/tmp/sdmsg";
    mkdir(cache.c_str(), 0755);

    /* Stable-ish name from basename + size if possible */
    std::string base = target;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    for (char &c : base) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
            c = '_';
    }
    return cache + "/" + base + ".sqlite";
}

} /* namespace sdmsg */
