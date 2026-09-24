/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mount/fuse_user.hpp"
#include "mount/host_priv.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace sdmsg {

namespace {

bool path_exists(const char *p) {
    struct stat st {};
    return ::stat(p, &st) == 0;
}

bool has_setuid(const char *path) {
    struct stat st {};
    if (::stat(path, &st) != 0)
        return false;
    return (st.st_mode & S_ISUID) != 0;
}

bool has_cap_sys_admin(const char *path) {
    const char *getcap = path_exists("/sbin/getcap")     ? "/sbin/getcap"
                         : path_exists("/usr/sbin/getcap") ? "/usr/sbin/getcap"
                                                           : "getcap";
    std::string cmd = std::string(getcap) + " " + path + " 2>/dev/null";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp)
        return false;
    char buf[512];
    std::string out;
    while (fgets(buf, sizeof buf, fp))
        out += buf;
    pclose(fp);
    return out.find("cap_sys_admin") != std::string::npos;
}

bool group_exists(const char *name) { return ::getgrnam(name) != nullptr; }

bool user_in_group(const char *gname) {
    gid_t gid;
    {
        struct group *g = ::getgrnam(gname);
        if (!g)
            return false;
        gid = g->gr_gid;
    }
    if (::getgid() == gid || ::getegid() == gid)
        return true;
    int n = ::getgroups(0, nullptr);
    if (n < 0)
        return false;
    std::vector<gid_t> gids(static_cast<size_t>(n));
    n = ::getgroups(n, gids.data());
    for (int i = 0; i < n; ++i) {
        if (gids[static_cast<size_t>(i)] == gid)
            return true;
    }
    struct group *g = ::getgrnam(gname);
    if (!g || !g->gr_mem)
        return false;
    const char *user = nullptr;
    if (const char *e = std::getenv("USER"))
        user = e;
    else if (struct passwd *pw = ::getpwuid(::geteuid()))
        user = pw->pw_name;
    if (!user)
        return false;
    for (char **m = g->gr_mem; *m; ++m) {
        if (std::strcmp(*m, user) == 0)
            return true;
    }
    return false;
}

const char *current_user() {
    if (const char *e = std::getenv("USER"))
        if (e[0])
            return e;
    if (struct passwd *pw = ::getpwuid(::geteuid()))
        return pw->pw_name;
    return nullptr;
}

std::string shell_quote(const std::string &s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    q += "'";
    return q;
}

int run_cmd_capture(const std::string &cmd, std::string *out) {
    std::string full = cmd + " 2>&1";
    FILE *fp = popen(full.c_str(), "r");
    if (!fp)
        return -1;
    char buf[512];
    std::string captured;
    while (fgets(buf, sizeof buf, fp))
        captured += buf;
    int rc = pclose(fp);
    if (out)
        *out = captured;
    if (rc < 0)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return -1;
}

bool write_temp_script(const std::string &body, std::string *path_out, std::string *err) {
    char tmpl[] = "/tmp/sdmsg-fuse-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        if (err)
            *err = "could not create temporary script";
        return false;
    }
    std::string content = "#!/bin/bash\n" + body + "\n";
    if (::write(fd, content.data(), content.size()) < 0) {
        ::close(fd);
        ::unlink(tmpl);
        if (err)
            *err = "could not write temporary script";
        return false;
    }
    ::fchmod(fd, 0700);
    ::close(fd);
    *path_out = tmpl;
    return true;
}

bool run_privileged_script(const std::string &script, std::string *err) {
    /* Prefer the host-namespace agent — pkexec fails inside our userns. */
    if (host_priv_agent_running())
        return run_host_privileged_script(script, err);

    std::string path;
    if (!write_temp_script(script, &path, err))
        return false;

    std::string output;
    int rc = run_cmd_capture("pkexec /bin/bash " + shell_quote(path), &output);
    /* No sudo fallback here: headless without an agent may still need sudo via
     * the agent path; GUI must never spawn a invisible sudo password prompt. */
    ::unlink(path.c_str());

    if (rc != 0) {
        if (err) {
            std::string msg = "Administrator approval failed";
            if (!output.empty()) {
                auto pos = output.find_last_not_of(" \t\r\n");
                if (pos != std::string::npos) {
                    auto start = output.find_last_of('\n', pos);
                    std::string line = output.substr(start == std::string::npos ? 0 : start + 1,
                                                     pos - (start == std::string::npos ? 0 : start));
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    if (!line.empty())
                        msg += ": " + line;
                }
            } else {
                msg += " (pkexec unavailable or cancelled)";
            }
            *err = msg;
        }
        return false;
    }
    return true;
}

const char *pick_setcap() {
    if (path_exists("/sbin/setcap"))
        return "/sbin/setcap";
    if (path_exists("/usr/sbin/setcap"))
        return "/usr/sbin/setcap";
    return "setcap";
}

const char *kHelpers =
    "/bin/fusermount3 /usr/bin/fusermount3 /bin/fusermount /usr/bin/fusermount";

bool desire_matches(const FuseUserMountDesire &d, const FuseUserMountStatus &s) {
    if (d.fuse_group && !s.fuse_group_exists)
        return false;
    if (d.member != s.user_in_fuse_group)
        return false;
    if (d.setuid != s.fusermount_setuid)
        return false;
    if (d.cap != s.fusermount_cap)
        return false;
    return true;
}

} /* namespace */

FuseUserMountStatus probe_fuse_user_mounts() {
    FuseUserMountStatus s;
    s.fuse_group_exists = group_exists("fuse");
    s.user_in_fuse_group = user_in_group("fuse");

    const char *helpers[] = {"/bin/fusermount3", "/usr/bin/fusermount3", "/bin/fusermount",
                             "/usr/bin/fusermount", nullptr};
    for (int i = 0; helpers[i]; ++i) {
        if (!path_exists(helpers[i]))
            continue;
        if (s.fusermount_path.empty())
            s.fusermount_path = helpers[i];
        if (has_setuid(helpers[i]))
            s.fusermount_setuid = true;
        if (has_cap_sys_admin(helpers[i]))
            s.fusermount_cap = true;
    }
    return s;
}

bool apply_fuse_user_mounts(const FuseUserMountDesire &desire, std::string *err) {
    const char *user = current_user();
    if (!user || !user[0]) {
        if (err)
            *err = "cannot determine current user name";
        return false;
    }

    auto cur = probe_fuse_user_mounts();
    if (desire_matches(desire, cur)) {
        if (err)
            err->clear();
        return true; /* nothing to change */
    }

    const char *setcap = pick_setcap();
    std::string script;
    script += "set -e\n";
    script += "PATH=/usr/sbin:/sbin:/usr/bin:/bin\n";
    if (desire.fuse_group)
        script += "groupadd -f fuse || true\n";

    if (desire.member) {
        script += std::string("usermod -aG fuse ") + user + "\n";
    } else {
        script += std::string("gpasswd -d ") + user + " fuse 2>/dev/null || true\n";
        script += std::string("deluser ") + user + " fuse 2>/dev/null || true\n";
    }

    script += "for f in ";
    script += kHelpers;
    script += "; do\n";
    script += "  [ -f \"$f\" ] || continue\n";
    if (desire.setuid)
        script += "  chmod u+s \"$f\" || true\n";
    else
        script += "  chmod u-s \"$f\" || true\n";
    if (desire.cap) {
        script += "  ";
        script += setcap;
        script += " cap_sys_admin+ep \"$f\" || true\n";
    } else {
        script += "  ";
        script += setcap;
        script += " -r \"$f\" 2>/dev/null || true\n";
    }
    script += "done\n";

    if (!run_privileged_script(script, err))
        return false;

    auto st = probe_fuse_user_mounts();
    if (desire.member && !st.user_in_fuse_group) {
        if (err)
            *err = "Group membership was updated. Sign out and back in (or reboot) "
                   "before it takes effect.";
    }
    return true;
}

bool disable_fuse_user_mounts(std::string *err) {
    FuseUserMountDesire d;
    d.fuse_group = true;
    d.member = false;
    d.setuid = false;
    d.cap = false;
    return apply_fuse_user_mounts(d, err);
}

} /* namespace sdmsg */
