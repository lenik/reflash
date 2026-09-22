/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "engine/sha1_pass.hpp"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <vector>

namespace sdmsg {

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool sha1_file(const std::string &path, unsigned char out[20], std::string *err) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err)
            *err = strerror(errno);
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
        if (ctx)
            EVP_MD_CTX_free(ctx);
        close(fd);
        if (err)
            *err = "EVP_DigestInit_ex failed";
        return false;
    }
    unsigned char buf[1 << 16];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (err)
                *err = strerror(errno);
            EVP_MD_CTX_free(ctx);
            close(fd);
            return false;
        }
        if (n == 0)
            break;
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n));
    }
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
    close(fd);
    return true;
}

void walk_dir(const std::string &root, const std::string &rel,
              std::map<std::string, std::string> &files) {
    std::string dirpath = rel.empty() ? root : root + "/" + rel;
    DIR *d = opendir(dirpath.c_str());
    if (!d)
        return;
    while (auto *ent = readdir(d)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
            continue;
        std::string name = ent->d_name;
        std::string child_rel = rel.empty() ? name : rel + "/" + name;
        std::string full = root + "/" + child_rel;
        struct stat st {};
        if (lstat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk_dir(root, child_rel, files);
        else if (S_ISREG(st.st_mode))
            files["/" + child_rel] = full;
    }
    closedir(d);
}

} /* namespace */

bool sha1_mounted_files(Store &store, const std::vector<MountRecord> &mounts, Progress &progress,
                        bool verify_mode, std::string *err) {
    std::map<std::string, std::string> files; /* db-style path -> absolute */
    for (const auto &m : mounts)
        walk_dir(m.target, "", files);

    auto objects = store.list_objects();
    /* Also hash every file found on the mount, even if not in DB yet. */
    std::map<std::string, std::string> todo = files;
    for (const auto &o : objects) {
        if (o.path.size() >= 2 && o.path.compare(0, 2, "::") == 0)
            continue;
        if (todo.find(o.path) == todo.end()) {
            /* try case-insensitive match */
            std::string want = lower(o.path);
            for (const auto &kv : files) {
                if (lower(kv.first) == want) {
                    todo[o.path] = kv.second;
                    break;
                }
            }
        }
    }

    progress.set_phase("sha1");
    std::uint64_t n = 0;
    for (const auto &kv : todo) {
        (void)kv;
        n++;
    }
    /* Progress is byte-oriented; use file count as pseudo-bytes. */
    progress.reset(n ? n : 1, 1);

    for (const auto &kv : todo) {
        const std::string &db_path = kv.first;
        const std::string &abs = kv.second;
        unsigned char dig[20];
        std::string e;
        progress.set_phase("sha1 " + db_path);
        if (!sha1_file(abs, dig, &e)) {
            progress.log(0, 0, CellStatus::ReadErr, "sha1 " + db_path + ": " + e);
            if (verify_mode)
                store.bump_verify(db_path, VerifyStatus::Fail, nullptr);
            progress.add_done(1);
            continue;
        }
        struct stat st {};
        std::int64_t sz = 0;
        if (stat(abs.c_str(), &st) == 0)
            sz = st.st_size;

        ObjectRecord prev;
        bool have = store.get_object(db_path, prev);
        if (!have) {
            ObjectRecord o;
            o.path = db_path;
            o.size = sz;
            store.upsert_object(o);
            have = store.get_object(db_path, prev);
        }

        if (verify_mode) {
            VerifyStatus stv = VerifyStatus::Ok;
            if (have && prev.has_sha1 && std::memcmp(prev.sha1, dig, 20) != 0)
                stv = VerifyStatus::Fail;
            store.bump_verify(db_path, stv, dig);
        } else {
            store.set_sha1(db_path, dig);
            /* keep size fresh */
            ObjectRecord o;
            if (store.get_object(db_path, o)) {
                o.size = sz;
                o.has_sha1 = true;
                std::memcpy(o.sha1, dig, 20);
                store.upsert_object(o);
            }
        }
        progress.add_done(1);
    }
    (void)err;
    return true;
}

} /* namespace sdmsg */
