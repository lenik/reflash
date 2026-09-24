/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "db/store.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

namespace sdmsg {

namespace {

constexpr int kDefaultSha1Days = 183; /* about half a year */

void bind_sha1(sqlite3_stmt *st, int idx, const ObjectRecord &o) {
    if (o.has_sha1)
        sqlite3_bind_blob(st, idx, o.sha1, 20, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, idx);
}

void read_sha1(sqlite3_stmt *st, int idx, unsigned char out[20], bool &has) {
    const void *blob = sqlite3_column_blob(st, idx);
    int blen = sqlite3_column_bytes(st, idx);
    has = blob && blen == 20;
    if (has)
        std::memcpy(out, blob, 20);
}

bool special_path(const std::string &path) {
    return path.size() >= 2 && path[0] == ':' && path[1] == ':';
}

void split_file_path(const std::string &path, std::vector<std::string> &dirs, std::string &base) {
    std::string p = path;
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty())
                parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        parts.push_back(cur);
    if (parts.empty()) {
        base.clear();
        return;
    }
    base = parts.back();
    parts.pop_back();
    dirs.swap(parts);
}

} /* namespace */

Store::~Store() { close(); }

void Store::close() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
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

bool Store::table_exists(const char *name) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st,
                           nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    bool yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

bool Store::migrate(std::string *err) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS device ("
        "  id INTEGER PRIMARY KEY,"
        "  vendor TEXT NOT NULL DEFAULT '',"
        "  product TEXT NOT NULL DEFAULT '',"
        "  serial TEXT NOT NULL DEFAULT '',"
        "  name TEXT NOT NULL DEFAULT '',"
        "  description TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS volume ("
        "  id INTEGER PRIMARY KEY,"
        "  dev_id INTEGER NOT NULL REFERENCES device(id),"
        "  type TEXT NOT NULL DEFAULT '',"
        "  uuid TEXT NOT NULL DEFAULT '',"
        "  label TEXT NOT NULL DEFAULT '',"
        "  name TEXT NOT NULL DEFAULT '',"
        "  description TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS object ("
        "  id INTEGER PRIMARY KEY,"
        "  vol_id INTEGER NOT NULL REFERENCES volume(id),"
        "  type TEXT NOT NULL,"
        "  sha1 BLOB,"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  last_massage INTEGER NOT NULL DEFAULT 0,"
        "  massage_count INTEGER NOT NULL DEFAULT 0,"
        "  verify_status INTEGER NOT NULL DEFAULT 0,"
        "  verify_count INTEGER NOT NULL DEFAULT 0,"
        "  verified_at INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(vol_id, type)"
        ");"
        "CREATE TABLE IF NOT EXISTS folder ("
        "  id INTEGER PRIMARY KEY,"
        "  vol_id INTEGER NOT NULL REFERENCES volume(id),"
        "  parent_id INTEGER NOT NULL DEFAULT 0,"
        "  basename TEXT NOT NULL,"
        "  UNIQUE(vol_id, parent_id, basename)"
        ");"
        "CREATE TABLE IF NOT EXISTS file ("
        "  id INTEGER PRIMARY KEY,"
        "  folder_id INTEGER NOT NULL REFERENCES folder(id),"
        "  basename TEXT NOT NULL,"
        "  sha1 BLOB,"
        "  size INTEGER NOT NULL DEFAULT 0,"
        "  verify_status INTEGER NOT NULL DEFAULT 0,"
        "  verify_count INTEGER NOT NULL DEFAULT 0,"
        "  verified_at INTEGER NOT NULL DEFAULT 0,"
        "  last_massage INTEGER NOT NULL DEFAULT 0,"
        "  massage_count INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(folder_id, basename)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_folder_parent ON folder(vol_id, parent_id);"
        "CREATE INDEX IF NOT EXISTS idx_file_folder ON file(folder_id);"
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
    if (!exec(ddl, err))
        return false;
    exec("PRAGMA journal_mode=WAL;", nullptr);
    import_legacy_objects();
    return true;
}

void Store::import_legacy_objects() {
    if (!table_exists("objects"))
        return;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT path,size,sha1,last_massage,massage_count,verify_status,verify_count"
                           " FROM objects",
                           -1, &st, nullptr) != SQLITE_OK)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        ObjectRecord o;
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        o.path = p ? p : "";
        o.size = sqlite3_column_int64(st, 1);
        read_sha1(st, 2, o.sha1, o.has_sha1);
        o.last_massage = sqlite3_column_int64(st, 3);
        o.massage_count = sqlite3_column_int64(st, 4);
        o.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
        o.verify_count = sqlite3_column_int64(st, 6);
        if (special_path(o.path))
            upsert_special_locked(o, nullptr);
        else if (!o.path.empty())
            upsert_file_locked(o, nullptr);
    }
    sqlite3_finalize(st);
    exec("ALTER TABLE objects RENAME TO objects_legacy;", nullptr);
}

bool Store::open(const std::string &path, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        path_.clear();
    }
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_ ? db_ : nullptr);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    if (!migrate(err)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    path_ = path;
    return true;
}

bool Store::check_integrity(std::string *report, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA integrity_check", -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    std::string out;
    bool ok = true;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        if (!t)
            continue;
        if (!out.empty())
            out += '\n';
        out += t;
        if (std::strcmp(t, "ok") != 0)
            ok = false;
    }
    sqlite3_finalize(st);
    if (report)
        *report = out.empty() ? "(empty)" : out;
    return ok;
}

namespace {

std::int64_t scalar_i64(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = nullptr;
    std::int64_t v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        return 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

double now_monotonic() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

} /* namespace */

bool Store::validate_manifest(ValidateProgressFn on_progress, std::atomic<bool> *pause,
                              std::atomic<bool> *cancel, std::string *report, std::string *err) {
    /* Hold the mutex for the whole run so other writers wait; UI still updates via callback. */
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }

    auto cancelled = [&]() {
        return cancel && cancel->load(std::memory_order_acquire);
    };
    auto wait_pause = [&]() {
        while (pause && pause->load(std::memory_order_acquire)) {
            if (cancelled())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return !cancelled();
    };

    ValidateStatus st;
    auto emit = [&](const char *phase, std::uint64_t done, std::uint64_t total, double eta) {
        st.phase = phase ? phase : "";
        st.done = done;
        st.total = total;
        st.eta_seconds = eta;
        st.finished = false;
        st.failed = false;
        st.cancelled = false;
        if (on_progress)
            on_progress(st);
    };

    auto finish = [&](bool ok, bool was_cancel, const std::string &rep) {
        st.finished = true;
        st.failed = !ok && !was_cancel;
        st.cancelled = was_cancel;
        if (was_cancel)
            st.phase = "Cancelled";
        else if (ok)
            st.phase = "Finished";
        else
            st.phase = "Failed";
        if (on_progress)
            on_progress(st);
        if (report)
            *report = rep;
        return ok && !was_cancel;
    };

    if (!wait_pause())
        return finish(false, true, "cancelled");

    emit("Counting catalog rows…", 0, 1, -1);
    const std::int64_t n_file = scalar_i64(db_, "SELECT COUNT(*) FROM file");
    const std::int64_t n_folder = scalar_i64(db_, "SELECT COUNT(*) FROM folder");
    const std::int64_t n_ext = scalar_i64(db_, "SELECT COUNT(*) FROM extents");
    const std::int64_t n_obj = table_exists("object") ? scalar_i64(db_, "SELECT COUNT(*) FROM object")
                                                     : 0;

    std::string summary;
    summary += "Files: " + std::to_string(n_file) + "\n";
    summary += "Folders: " + std::to_string(n_folder) + "\n";
    summary += "Extents: " + std::to_string(n_ext) + "\n";
    if (n_obj)
        summary += "Legacy objects: " + std::to_string(n_obj) + "\n";

    /* Weights for ETA: cross-ref batches + quick + full check. */
    const std::uint64_t w_folder = static_cast<std::uint64_t>(std::max<std::int64_t>(n_folder, 1));
    const std::uint64_t w_file = static_cast<std::uint64_t>(std::max<std::int64_t>(n_file, 1));
    const std::uint64_t w_ext = static_cast<std::uint64_t>(std::max<std::int64_t>(n_ext, 1));
    const std::uint64_t w_quick = 1000;
    const std::uint64_t w_full = 5000;
    const std::uint64_t total =
        w_folder + w_file + w_ext + w_quick + w_full;
    std::uint64_t done = 0;
    const double t0 = now_monotonic();
    auto eta_for = [&](std::uint64_t d) -> double {
        double elapsed = now_monotonic() - t0;
        if (d == 0 || elapsed < 0.2)
            return -1.0;
        double rate = static_cast<double>(d) / elapsed;
        if (rate < 1.0)
            return -1.0;
        return static_cast<double>(total - d) / rate;
    };

    auto bump = [&](std::uint64_t n) {
        done += n;
        if (done > total)
            done = total;
    };

    std::uint64_t issues = 0;
    constexpr int kBatch = 5000;

    auto scan_orphans = [&](const char *phase, const char *sql, std::uint64_t weight,
                            std::int64_t row_count) -> bool {
        if (!wait_pause())
            return false;
        emit(phase, done, total, eta_for(done));
        sqlite3_stmt *q = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &q, nullptr) != SQLITE_OK) {
            if (err)
                *err = std::string(phase) + ": " + sqlite3_errmsg(db_);
            return false;
        }
        std::int64_t seen = 0;
        while (sqlite3_step(q) == SQLITE_ROW) {
            if (cancelled()) {
                sqlite3_finalize(q);
                return false;
            }
            if (!wait_pause()) {
                sqlite3_finalize(q);
                return false;
            }
            ++issues;
            ++seen;
            if ((seen % 64) == 0) {
                std::uint64_t part =
                    row_count > 0
                        ? (weight * static_cast<std::uint64_t>(std::min(seen, row_count))) /
                              static_cast<std::uint64_t>(row_count)
                        : weight;
                emit(phase, done + part, total, eta_for(done + part));
            }
        }
        sqlite3_finalize(q);
        bump(weight);
        emit(phase, done, total, eta_for(done));
        (void)kBatch;
        return true;
    };

    auto orphan_fail = [&](const char *phase) -> bool {
        if (cancelled())
            return finish(false, true, summary + "\nCancelled during " + std::string(phase) + ".");
        std::string detail = (err && !err->empty()) ? *err : "check failed";
        return finish(false, false, summary + "\n" + detail);
    };

    if (!scan_orphans("Checking folder links…",
                      "SELECT f.id FROM folder f LEFT JOIN folder p ON p.id=f.parent_id "
                      "WHERE f.parent_id!=0 AND p.id IS NULL",
                      w_folder, n_folder))
        return orphan_fail("folder check");

    if (!scan_orphans("Checking file folder links…",
                      "SELECT file.id FROM file LEFT JOIN folder ON folder.id=file.folder_id "
                      "WHERE folder.id IS NULL",
                      w_file, n_file))
        return orphan_fail("file check");

    {
        /* file has folder_id+basename (no path); rebuild paths. Folders cover
         * root "/" (not stored as a file row). object.type holds special paths. */
        std::string ext_sql =
            "WITH RECURSIVE folder_path(id, rel) AS ("
            "  SELECT id, CAST('' AS TEXT) FROM folder WHERE parent_id=0"
            "  UNION ALL"
            "  SELECT c.id,"
            "         CASE WHEN p.rel='' THEN c.basename ELSE p.rel || '/' || c.basename END"
            "  FROM folder c JOIN folder_path p ON c.parent_id=p.id"
            "  WHERE c.parent_id!=0"
            "),"
            "known_paths(path) AS ("
            "  SELECT CASE WHEN fp.rel='' THEN '/' || f.basename"
            "              ELSE '/' || fp.rel || '/' || f.basename END"
            "  FROM file f JOIN folder_path fp ON f.folder_id=fp.id"
            "  UNION ALL"
            "  SELECT CASE WHEN rel='' THEN '/' ELSE '/' || rel END FROM folder_path";
        if (table_exists("object"))
            ext_sql += "  UNION ALL SELECT type FROM object";
        ext_sql +=
            ")"
            "SELECT e.id FROM extents e"
            " WHERE NOT EXISTS (SELECT 1 FROM known_paths k WHERE k.path=e.path)";
        if (!scan_orphans("Checking extent links…", ext_sql.c_str(), w_ext, n_ext))
            return orphan_fail("extent check");
    }

    if (!wait_pause())
        return finish(false, true, summary + "\nCancelled.");

    /* Progress handler so Cancel can interrupt long PRAGMAs. */
    struct ProgCtx {
        std::atomic<bool> *cancel = nullptr;
        Store::ValidateProgressFn *fn = nullptr;
        ValidateStatus *st = nullptr;
        std::uint64_t base = 0;
        std::uint64_t weight = 0;
        std::uint64_t total = 0;
        double t0 = 0;
        int ticks = 0;
    } pctx;
    pctx.cancel = cancel;
    pctx.fn = &on_progress;
    pctx.st = &st;
    pctx.base = done;
    pctx.weight = w_quick;
    pctx.total = total;
    pctx.t0 = t0;

    sqlite3_progress_handler(
        db_, 1000,
        [](void *p) -> int {
            auto *c = static_cast<ProgCtx *>(p);
            if (c->cancel && c->cancel->load(std::memory_order_acquire))
                return 1;
            ++c->ticks;
            std::uint64_t part = std::min(c->weight, static_cast<std::uint64_t>(c->ticks) * 10);
            c->st->done = c->base + part;
            c->st->total = c->total;
            double elapsed = now_monotonic() - c->t0;
            if (c->st->done > 0 && elapsed > 0.2)
                c->st->eta_seconds =
                    static_cast<double>(c->total - c->st->done) /
                    (static_cast<double>(c->st->done) / elapsed);
            if (c->fn && *c->fn)
                (*c->fn)(*c->st);
            return 0;
        },
        &pctx);

    emit("Quick database check…", done, total, eta_for(done));
    {
        sqlite3_stmt *q = nullptr;
        bool ok = true;
        std::string qout;
        if (sqlite3_prepare_v2(db_, "PRAGMA quick_check", -1, &q, nullptr) == SQLITE_OK) {
            while (sqlite3_step(q) == SQLITE_ROW) {
                if (cancelled())
                    break;
                const char *t = reinterpret_cast<const char *>(sqlite3_column_text(q, 0));
                if (!t)
                    continue;
                if (!qout.empty())
                    qout += '\n';
                qout += t;
                if (std::strcmp(t, "ok") != 0)
                    ok = false;
            }
            sqlite3_finalize(q);
        }
        if (cancelled()) {
            sqlite3_progress_handler(db_, 0, nullptr, nullptr);
            return finish(false, true, summary + "\nCancelled during quick check.");
        }
        if (!ok) {
            sqlite3_progress_handler(db_, 0, nullptr, nullptr);
            summary += "\nQuick check:\n" + qout;
            return finish(false, false, summary);
        }
        summary += "\nQuick check: ok\n";
    }
    bump(w_quick);

    if (!wait_pause()) {
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        return finish(false, true, summary + "\nCancelled.");
    }

    pctx.base = done;
    pctx.weight = w_full;
    pctx.ticks = 0;
    st.phase = "Full integrity check…";
    emit("Full integrity check…", done, total, eta_for(done));
    {
        sqlite3_stmt *q = nullptr;
        bool ok = true;
        std::string iout;
        if (sqlite3_prepare_v2(db_, "PRAGMA integrity_check", -1, &q, nullptr) == SQLITE_OK) {
            while (sqlite3_step(q) == SQLITE_ROW) {
                if (cancelled())
                    break;
                const char *t = reinterpret_cast<const char *>(sqlite3_column_text(q, 0));
                if (!t)
                    continue;
                if (!iout.empty())
                    iout += '\n';
                iout += t;
                if (std::strcmp(t, "ok") != 0)
                    ok = false;
            }
            sqlite3_finalize(q);
        }
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        if (cancelled())
            return finish(false, true, summary + "\nCancelled during integrity check.");
        summary += "Integrity check: " + (iout.empty() ? "(empty)" : iout) + "\n";
        if (!ok)
            return finish(false, false, summary);
    }
    bump(w_full);
    done = total;

    if (issues)
        summary += "Orphan / broken links found: " + std::to_string(issues) + "\n";
    else
        summary += "Cross-references: ok\n";

    bool ok = issues == 0;
    emit("Finished", done, total, 0);
    return finish(ok, false, summary);
}

std::int64_t Store::ensure_volume_locked() {
    std::string id;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key='current_vol_id'", -1, &st,
                           nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
            id = v ? v : "";
        }
        sqlite3_finalize(st);
    }
    if (!id.empty()) {
        std::int64_t vol = std::strtoll(id.c_str(), nullptr, 10);
        if (vol > 0)
            return vol;
    }
    exec("INSERT INTO device(vendor,product,serial,name,description) VALUES('','','','','');",
         nullptr);
    std::int64_t dev = sqlite3_last_insert_rowid(db_);
    sqlite3_stmt *ins = nullptr;
    sqlite3_prepare_v2(db_,
                       "INSERT INTO volume(dev_id,type,uuid,label,name,description)"
                       " VALUES(?,'','','','','')",
                       -1, &ins, nullptr);
    sqlite3_bind_int64(ins, 1, dev);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    std::int64_t vol = sqlite3_last_insert_rowid(db_);
    set_meta("current_vol_id", std::to_string(vol), nullptr);
    ensure_root_folder_locked(vol);
    return vol;
}

std::int64_t Store::ensure_root_folder_locked(std::int64_t vol) {
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(db_,
                       "SELECT id FROM folder WHERE vol_id=? AND parent_id=0 AND basename=''", -1,
                       &st, nullptr);
    sqlite3_bind_int64(st, 1, vol);
    if (sqlite3_step(st) == SQLITE_ROW) {
        std::int64_t id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return id;
    }
    sqlite3_finalize(st);
    sqlite3_stmt *ins = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO folder(vol_id,parent_id,basename) VALUES(?,0,'')", -1, &ins,
                       nullptr);
    sqlite3_bind_int64(ins, 1, vol);
    sqlite3_step(ins);
    std::int64_t id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(ins);
    return id;
}

std::int64_t Store::ensure_folder_locked(std::int64_t vol, std::int64_t parent,
                                         const std::string &basename, std::string *err) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id FROM folder WHERE vol_id=? AND parent_id=? AND basename=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return 0;
    }
    sqlite3_bind_int64(st, 1, vol);
    sqlite3_bind_int64(st, 2, parent);
    sqlite3_bind_text(st, 3, basename.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        std::int64_t id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return id;
    }
    sqlite3_finalize(st);
    sqlite3_stmt *ins = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO folder(vol_id,parent_id,basename) VALUES(?,?,?)", -1,
                           &ins, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return 0;
    }
    sqlite3_bind_int64(ins, 1, vol);
    sqlite3_bind_int64(ins, 2, parent);
    sqlite3_bind_text(ins, 3, basename.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE) {
        if (err)
            *err = sqlite3_errmsg(db_);
        sqlite3_finalize(ins);
        return 0;
    }
    std::int64_t id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(ins);
    return id;
}

bool Store::upsert_special_locked(const ObjectRecord &o, std::string *err) {
    std::int64_t vol = ensure_volume_locked();
    const char *sql =
        "INSERT INTO object(vol_id,type,sha1,size,last_massage,massage_count,"
        "verify_status,verify_count,verified_at) VALUES(?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(vol_id,type) DO UPDATE SET"
        " sha1=COALESCE(excluded.sha1, object.sha1),"
        " size=excluded.size,"
        " last_massage=excluded.last_massage,"
        " massage_count=excluded.massage_count,"
        " verify_status=excluded.verify_status,"
        " verify_count=excluded.verify_count,"
        " verified_at=excluded.verified_at";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, vol);
    sqlite3_bind_text(st, 2, o.path.c_str(), -1, SQLITE_TRANSIENT);
    bind_sha1(st, 3, o);
    sqlite3_bind_int64(st, 4, o.size);
    sqlite3_bind_int64(st, 5, o.last_massage);
    sqlite3_bind_int64(st, 6, o.massage_count);
    sqlite3_bind_int(st, 7, static_cast<int>(o.verify_status));
    sqlite3_bind_int64(st, 8, o.verify_count);
    sqlite3_bind_int64(st, 9, o.verified_at);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::upsert_file_locked(const ObjectRecord &o, std::string *err) {
    std::vector<std::string> dirs;
    std::string base;
    split_file_path(o.path, dirs, base);
    if (base.empty()) {
        if (err)
            *err = "empty file path";
        return false;
    }
    std::int64_t vol = ensure_volume_locked();
    std::int64_t parent = ensure_root_folder_locked(vol);
    for (const auto &d : dirs) {
        parent = ensure_folder_locked(vol, parent, d, err);
        if (parent <= 0)
            return false;
    }
    const char *sql =
        "INSERT INTO file(folder_id,basename,sha1,size,verify_status,verify_count,verified_at,"
        "last_massage,massage_count) VALUES(?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(folder_id,basename) DO UPDATE SET"
        " sha1=COALESCE(excluded.sha1, file.sha1),"
        " size=excluded.size,"
        " verify_status=excluded.verify_status,"
        " verify_count=excluded.verify_count,"
        " verified_at=excluded.verified_at,"
        " last_massage=excluded.last_massage,"
        " massage_count=excluded.massage_count";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, parent);
    sqlite3_bind_text(st, 2, base.c_str(), -1, SQLITE_TRANSIENT);
    bind_sha1(st, 3, o);
    sqlite3_bind_int64(st, 4, o.size);
    sqlite3_bind_int(st, 5, static_cast<int>(o.verify_status));
    sqlite3_bind_int64(st, 6, o.verify_count);
    sqlite3_bind_int64(st, 7, o.verified_at);
    sqlite3_bind_int64(st, 8, o.last_massage);
    sqlite3_bind_int64(st, 9, o.massage_count);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool Store::upsert_object(const ObjectRecord &o, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }
    if (special_path(o.path))
        return upsert_special_locked(o, err);
    return upsert_file_locked(o, err);
}

bool Store::get_special_locked(const std::string &path, ObjectRecord &out) {
    std::int64_t vol = ensure_volume_locked();
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT type,size,sha1,last_massage,massage_count,verify_status,"
                           "verify_count,verified_at FROM object WHERE vol_id=? AND type=?",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, vol);
    sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    const char *t = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
    out.path = t ? t : path;
    out.size = sqlite3_column_int64(st, 1);
    read_sha1(st, 2, out.sha1, out.has_sha1);
    out.last_massage = sqlite3_column_int64(st, 3);
    out.massage_count = sqlite3_column_int64(st, 4);
    out.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
    out.verify_count = sqlite3_column_int64(st, 6);
    out.verified_at = sqlite3_column_int64(st, 7);
    sqlite3_finalize(st);
    return true;
}

bool Store::get_file_locked(const std::string &path, ObjectRecord &out) {
    std::vector<std::string> dirs;
    std::string base;
    split_file_path(path, dirs, base);
    if (base.empty())
        return false;
    std::int64_t vol = ensure_volume_locked();
    std::int64_t parent = ensure_root_folder_locked(vol);
    for (const auto &d : dirs) {
        sqlite3_stmt *st = nullptr;
        sqlite3_prepare_v2(db_,
                           "SELECT id FROM folder WHERE vol_id=? AND parent_id=? AND basename=?",
                           -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, vol);
        sqlite3_bind_int64(st, 2, parent);
        sqlite3_bind_text(st, 3, d.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            return false;
        }
        parent = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT basename,size,sha1,last_massage,massage_count,verify_status,"
                           "verify_count,verified_at FROM file WHERE folder_id=? AND basename=?",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, parent);
    sqlite3_bind_text(st, 2, base.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.path = path;
    out.size = sqlite3_column_int64(st, 1);
    read_sha1(st, 2, out.sha1, out.has_sha1);
    out.last_massage = sqlite3_column_int64(st, 3);
    out.massage_count = sqlite3_column_int64(st, 4);
    out.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
    out.verify_count = sqlite3_column_int64(st, 6);
    out.verified_at = sqlite3_column_int64(st, 7);
    sqlite3_finalize(st);
    return true;
}

bool Store::get_object(const std::string &path, ObjectRecord &out, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }
    bool ok = special_path(path) ? get_special_locked(path, out) : get_file_locked(path, out);
    (void)err;
    return ok;
}

bool Store::bump_massage(const std::string &path, std::int64_t size, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    ObjectRecord o;
    if (special_path(path)) {
        if (!get_special_locked(path, o))
            o.path = path;
    } else if (!get_file_locked(path, o)) {
        o.path = path;
    }
    o.size = size;
    o.last_massage = static_cast<std::int64_t>(std::time(nullptr));
    o.massage_count += 1;
    if (special_path(path))
        return upsert_special_locked(o, err);
    return upsert_file_locked(o, err);
}

bool Store::set_sha1(const std::string &path, const unsigned char sha1[20], std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    ObjectRecord o;
    bool have = special_path(path) ? get_special_locked(path, o) : get_file_locked(path, o);
    if (!have)
        o.path = path;
    std::memcpy(o.sha1, sha1, 20);
    o.has_sha1 = true;
    if (special_path(path))
        return upsert_special_locked(o, err);
    return upsert_file_locked(o, err);
}

bool Store::bump_verify(const std::string &path, VerifyStatus st, const unsigned char *sha1,
                        std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    ObjectRecord o;
    bool have = special_path(path) ? get_special_locked(path, o) : get_file_locked(path, o);
    if (!have)
        o.path = path;
    if (sha1) {
        std::memcpy(o.sha1, sha1, 20);
        o.has_sha1 = true;
    }
    o.verify_status = st;
    o.verify_count += 1;
    o.verified_at = static_cast<std::int64_t>(std::time(nullptr));
    if (special_path(path))
        return upsert_special_locked(o, err);
    return upsert_file_locked(o, err);
}

std::vector<ObjectRecord> Store::list_objects(std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<ObjectRecord> out;
    if (!db_) {
        if (err)
            *err = "database not open";
        return out;
    }
    std::int64_t vol = ensure_volume_locked();
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT type,size,sha1,last_massage,massage_count,verify_status,"
                           "verify_count,verified_at FROM object WHERE vol_id=? ORDER BY type",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, vol);
        while (sqlite3_step(st) == SQLITE_ROW) {
            ObjectRecord o;
            const char *t = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
            o.path = t ? t : "";
            o.size = sqlite3_column_int64(st, 1);
            read_sha1(st, 2, o.sha1, o.has_sha1);
            o.last_massage = sqlite3_column_int64(st, 3);
            o.massage_count = sqlite3_column_int64(st, 4);
            o.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
            o.verify_count = sqlite3_column_int64(st, 6);
            o.verified_at = sqlite3_column_int64(st, 7);
            out.push_back(o);
        }
        sqlite3_finalize(st);
    }
    const char *filesql =
        "WITH RECURSIVE tree AS ("
        "  SELECT id, basename, '' AS path FROM folder WHERE vol_id=? AND parent_id=0 AND basename=''"
        "  UNION ALL"
        "  SELECT f.id, f.basename,"
        "         CASE WHEN tree.path='' THEN f.basename ELSE tree.path || '/' || f.basename END"
        "  FROM folder f JOIN tree ON f.parent_id=tree.id"
        ")"
        " SELECT CASE WHEN tree.path='' THEN '/' || file.basename"
        "             ELSE '/' || tree.path || '/' || file.basename END,"
        "        file.size, file.sha1, file.last_massage, file.massage_count,"
        "        file.verify_status, file.verify_count, file.verified_at"
        " FROM file JOIN tree ON file.folder_id=tree.id"
        " ORDER BY 1";
    if (sqlite3_prepare_v2(db_, filesql, -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return out;
    }
    sqlite3_bind_int64(st, 1, vol);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ObjectRecord o;
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        o.path = p ? p : "";
        o.size = sqlite3_column_int64(st, 1);
        read_sha1(st, 2, o.sha1, o.has_sha1);
        o.last_massage = sqlite3_column_int64(st, 3);
        o.massage_count = sqlite3_column_int64(st, 4);
        o.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 5));
        o.verify_count = sqlite3_column_int64(st, 6);
        o.verified_at = sqlite3_column_int64(st, 7);
        out.push_back(o);
    }
    sqlite3_finalize(st);
    return out;
}

bool Store::replace_extents(const std::string &path, const std::vector<ExtentRecord> &exts,
                            std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    sqlite3_stmt *del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM extents WHERE path=?", -1, &del, nullptr) != SQLITE_OK) {
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
    std::lock_guard<std::recursive_mutex> lock(mu_);
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
    std::lock_guard<std::recursive_mutex> lock(mu_);
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
    std::lock_guard<std::recursive_mutex> lock(mu_);
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
        const char *p = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        e.path = p ? p : "";
        e.offset = sqlite3_column_int64(st, 1);
        e.length = sqlite3_column_int64(st, 2);
        e.status = sqlite3_column_int(st, 3);
        out.push_back(e);
    }
    sqlite3_finalize(st);
    return out;
}

int Store::sha1_valid_days() {
    std::string v;
    if (!get_meta("sha1_valid_days", v, nullptr) || v.empty())
        return kDefaultSha1Days;
    int n = std::atoi(v.c_str());
    return n > 0 ? n : kDefaultSha1Days;
}

bool Store::set_sha1_valid_days(int days, std::string *err) {
    if (days < 1)
        days = 1;
    return set_meta("sha1_valid_days", std::to_string(days), err);
}

std::int64_t Store::current_volume_id() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_)
        return 0;
    return ensure_volume_locked();
}

std::int64_t Store::ensure_folder(std::int64_t parent_id, const std::string &basename,
                                  std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return 0;
    }
    std::int64_t vol = ensure_volume_locked();
    if (parent_id <= 0)
        parent_id = ensure_root_folder_locked(vol);
    return ensure_folder_locked(vol, parent_id, basename, err);
}

std::vector<DirDbEntry> Store::list_db_dir(std::int64_t folder_id, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<DirDbEntry> out;
    if (!db_) {
        if (err)
            *err = "database not open";
        return out;
    }
    std::int64_t vol = ensure_volume_locked();
    std::int64_t root = ensure_root_folder_locked(vol);
    if (folder_id <= 0)
        folder_id = root;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id,basename FROM folder WHERE vol_id=? AND parent_id=? AND basename!=''"
                           " ORDER BY basename",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, vol);
        sqlite3_bind_int64(st, 2, folder_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            DirDbEntry e;
            e.directory = true;
            e.id = sqlite3_column_int64(st, 0);
            const char *n = reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
            e.basename = n ? n : "";
            out.push_back(e);
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db_,
                           "SELECT id,basename,size,sha1,verify_status,verified_at FROM file"
                           " WHERE folder_id=? ORDER BY basename",
                           -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return out;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DirDbEntry e;
        e.directory = false;
        e.id = sqlite3_column_int64(st, 0);
        const char *n = reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
        e.basename = n ? n : "";
        e.size = sqlite3_column_int64(st, 2);
        read_sha1(st, 3, e.sha1, e.has_sha1);
        e.verify_status = static_cast<VerifyStatus>(sqlite3_column_int(st, 4));
        e.verified_at = sqlite3_column_int64(st, 5);
        out.push_back(e);
    }
    sqlite3_finalize(st);
    return out;
}

std::int64_t Store::file_record_count(std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return 0;
    }
    return scalar_i64(db_, "SELECT COUNT(*) FROM file");
}

bool Store::delete_file(const std::string &path, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }
    if (special_path(path)) {
        std::int64_t vol = ensure_volume_locked();
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM object WHERE vol_id=? AND type=?", -1, &st,
                               nullptr) != SQLITE_OK) {
            if (err)
                *err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, vol);
        sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok && err)
            *err = sqlite3_errmsg(db_);
        replace_extents(path, {}, nullptr);
        return ok;
    }
    ObjectRecord o;
    if (!get_file_locked(path, o))
        return true;
    std::vector<std::string> dirs;
    std::string base;
    split_file_path(path, dirs, base);
    std::int64_t vol = ensure_volume_locked();
    std::int64_t parent = ensure_root_folder_locked(vol);
    for (const auto &d : dirs) {
        parent = ensure_folder_locked(vol, parent, d, err);
        if (parent <= 0)
            return false;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM file WHERE folder_id=? AND basename=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, parent);
    sqlite3_bind_text(st, 2, base.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    replace_extents(path, {}, nullptr);
    return ok;
}

bool Store::delete_folder(std::int64_t folder_id, std::string *err) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) {
        if (err)
            *err = "database not open";
        return false;
    }
    if (folder_id <= 0)
        return true;
    std::int64_t vol = ensure_volume_locked();
    std::int64_t root = ensure_root_folder_locked(vol);
    if (folder_id == root)
        return true;

    std::vector<std::int64_t> children;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM folder WHERE vol_id=? AND parent_id=?", -1, &st,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, vol);
        sqlite3_bind_int64(st, 2, folder_id);
        while (sqlite3_step(st) == SQLITE_ROW)
            children.push_back(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }
    for (std::int64_t c : children) {
        if (!delete_folder(c, err))
            return false;
    }

    if (sqlite3_prepare_v2(db_, "DELETE FROM file WHERE folder_id=?", -1, &st, nullptr) !=
        SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        if (err)
            *err = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        return false;
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db_, "DELETE FROM folder WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        if (err)
            *err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, folder_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok && err)
        *err = sqlite3_errmsg(db_);
    return ok;
}

std::string default_db_path(const std::string &target) {
    const char *home = getenv("HOME");
    std::string cache = home ? std::string(home) + "/.cache/sdmsg" : "/tmp/sdmsg";
    mkdir(cache.c_str(), 0755);

    std::string base = target;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    for (char &c : base) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
            c = '_';
    }
    return cache + "/" + base + ".sqlite";
}

} /* namespace sdmsg */
