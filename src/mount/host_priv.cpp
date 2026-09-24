/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mount/host_priv.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace reflash {

namespace {

std::mutex g_mu;
int g_req_w = -1;
int g_rep_r = -1;
pid_t g_pid = -1;
bool g_gui = false;

/* Snapshot of session env for polkit GUI (captured at agent start). */
std::string g_display;
std::string g_wayland;
std::string g_xauthority;
std::string g_dbus;
std::string g_xdg_runtime;

bool write_all(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (w == 0)
            return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool read_all(int fd, void *buf, size_t n) {
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void snap_session_env() {
    auto take = [](const char *k) -> std::string {
        const char *v = std::getenv(k);
        return v ? v : "";
    };
    g_display = take("DISPLAY");
    g_wayland = take("WAYLAND_DISPLAY");
    g_xauthority = take("XAUTHORITY");
    g_dbus = take("DBUS_SESSION_BUS_ADDRESS");
    g_xdg_runtime = take("XDG_RUNTIME_DIR");
}

std::string fixed_script_path() {
    return "/tmp/reflash-priv-" + std::to_string(static_cast<long>(::getuid())) + "/run.sh";
}

bool ensure_script_dir(std::string *err) {
    std::string dir = "/tmp/reflash-priv-" + std::to_string(static_cast<long>(::getuid()));
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (err)
            *err = std::string("mkdir: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool write_fixed_script(const std::string &body, std::string *path_out, std::string *err) {
    if (!ensure_script_dir(err))
        return false;
    std::string path = fixed_script_path();
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    if (fd < 0) {
        if (err)
            *err = std::string("open script: ") + std::strerror(errno);
        return false;
    }
    std::string content = "#!/bin/bash\nset -e\n" + body + "\n";
    if (::write(fd, content.data(), content.size()) < 0) {
        ::close(fd);
        if (err)
            *err = "could not write privileged script";
        return false;
    }
    ::fchmod(fd, 0700);
    ::close(fd);
    *path_out = path;
    return true;
}

/*
 * Run argv with fork/exec. Do not use popen — redirecting pkexec through a pipe
 * often makes the polkit dialog flash and vanish. Capture output in a temp file.
 */
int run_argv_capture(const std::vector<std::string> &argv, std::string *out) {
    if (argv.empty())
        return -1;
    char out_tmpl[] = "/tmp/reflash-priv-out-XXXXXX";
    int out_fd = ::mkstemp(out_tmpl);
    if (out_fd < 0)
        return -1;
    ::close(out_fd);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::unlink(out_tmpl);
        return -1;
    }
    if (pid == 0) {
        int fd = ::open(out_tmpl, O_WRONLY | O_TRUNC);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        int nullfd = ::open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            ::dup2(nullfd, STDIN_FILENO);
            ::close(nullfd);
        }
        /* Restore session env so the polkit agent can show a GUI prompt. */
        if (!g_display.empty())
            ::setenv("DISPLAY", g_display.c_str(), 1);
        if (!g_wayland.empty())
            ::setenv("WAYLAND_DISPLAY", g_wayland.c_str(), 1);
        if (!g_xauthority.empty())
            ::setenv("XAUTHORITY", g_xauthority.c_str(), 1);
        if (!g_dbus.empty())
            ::setenv("DBUS_SESSION_BUS_ADDRESS", g_dbus.c_str(), 1);
        if (!g_xdg_runtime.empty())
            ::setenv("XDG_RUNTIME_DIR", g_xdg_runtime.c_str(), 1);

        std::vector<char *> av;
        av.reserve(argv.size() + 1);
        for (const auto &s : argv)
            av.push_back(const_cast<char *>(s.c_str()));
        av.push_back(nullptr);
        ::execvp(av[0], av.data());
        ::_exit(127);
    }

    int st = 0;
    if (::waitpid(pid, &st, 0) < 0) {
        ::unlink(out_tmpl);
        return -1;
    }
    FILE *fp = ::fopen(out_tmpl, "r");
    if (fp) {
        char buf[512];
        std::string captured;
        while (std::fgets(buf, sizeof buf, fp))
            captured += buf;
        ::fclose(fp);
        if (out)
            *out = captured;
    }
    ::unlink(out_tmpl);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}

int run_script_host(const std::string &script, std::string *output) {
    std::string path;
    std::string err;
    if (!write_fixed_script(script, &path, &err)) {
        if (output)
            *output = err;
        return -1;
    }

    /* Stable script path helps polkit remember recent authorization. */
    std::vector<std::string> pk = {"pkexec", "/bin/bash", path};
    int rc = run_argv_capture(pk, output);
    if (rc == 0)
        return 0;

    if (g_gui) {
        /* GUI: never fall back to sudo — the password prompt is invisible. */
        return rc != 0 ? rc : -1;
    }

    std::vector<std::string> sudo_n = {"sudo", "-n", "/bin/bash", path};
    rc = run_argv_capture(sudo_n, output);
    if (rc == 0)
        return 0;
    std::vector<std::string> sudo = {"sudo", "/bin/bash", path};
    return run_argv_capture(sudo, output);
}

void agent_main(int req_r, int rep_w) {
    for (;;) {
        std::uint32_t len = 0;
        if (!read_all(req_r, &len, sizeof len))
            break;
        if (len == 0xffffffffu)
            break;
        if (len > 8u * 1024u * 1024u)
            break;
        std::string script(len, '\0');
        if (len > 0 && !read_all(req_r, script.data(), len))
            break;

        std::string output;
        int rc = run_script_host(script, &output);
        if (output.size() > 1024u * 1024u)
            output.resize(1024u * 1024u);

        std::int32_t irc = static_cast<std::int32_t>(rc);
        std::uint32_t olen = static_cast<std::uint32_t>(output.size());
        if (!write_all(rep_w, &irc, sizeof irc) || !write_all(rep_w, &olen, sizeof olen))
            break;
        if (olen > 0 && !write_all(rep_w, output.data(), olen))
            break;
    }
    ::_exit(0);
}

} /* namespace */

void set_host_priv_gui(bool gui) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_gui = gui;
}

bool host_priv_gui() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_gui;
}

bool start_host_priv_agent() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_pid > 0)
        return true;

    snap_session_env();

    int req[2] = {-1, -1};
    int rep[2] = {-1, -1};
    if (::pipe2(req, O_CLOEXEC) != 0 || ::pipe2(rep, O_CLOEXEC) != 0) {
        if (req[0] >= 0)
            ::close(req[0]);
        if (req[1] >= 0)
            ::close(req[1]);
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(req[0]);
        ::close(req[1]);
        ::close(rep[0]);
        ::close(rep[1]);
        return false;
    }
    if (pid == 0) {
        ::close(req[1]);
        ::close(rep[0]);
        agent_main(req[0], rep[1]);
        ::_exit(0);
    }

    ::close(req[0]);
    ::close(rep[1]);
    g_req_w = req[1];
    g_rep_r = rep[0];
    g_pid = pid;
    return true;
}

bool host_priv_agent_running() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_pid > 0 && g_req_w >= 0 && g_rep_r >= 0;
}

bool run_host_privileged_script(const std::string &script, std::string *err) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_pid <= 0 || g_req_w < 0 || g_rep_r < 0) {
        if (err)
            *err = "host privilege agent is not running";
        return false;
    }

    std::uint32_t len = static_cast<std::uint32_t>(script.size());
    if (!write_all(g_req_w, &len, sizeof len) ||
        (len > 0 && !write_all(g_req_w, script.data(), len))) {
        if (err)
            *err = "failed to send privileged script to host agent";
        return false;
    }

    std::int32_t rc = -1;
    std::uint32_t olen = 0;
    if (!read_all(g_rep_r, &rc, sizeof rc) || !read_all(g_rep_r, &olen, sizeof olen)) {
        if (err)
            *err = "host privilege agent disconnected";
        return false;
    }
    std::string output;
    if (olen > 0) {
        if (olen > 1024u * 1024u) {
            if (err)
                *err = "host privilege agent reply too large";
            return false;
        }
        output.resize(olen);
        if (!read_all(g_rep_r, output.data(), olen)) {
            if (err)
                *err = "host privilege agent disconnected";
            return false;
        }
    }

    if (rc != 0) {
        if (err) {
            std::string msg = "Administrator approval failed";
            if (!output.empty()) {
                auto pos = output.find_last_not_of(" \t\r\n");
                if (pos != std::string::npos) {
                    auto start = output.find_last_of('\n', pos);
                    std::string line =
                        output.substr(start == std::string::npos ? 0 : start + 1,
                                      pos - (start == std::string::npos ? 0 : start));
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    if (!line.empty())
                        msg += ": " + line;
                }
            } else {
                msg += g_gui ? " (pkexec cancelled or unavailable)"
                             : " (pkexec/sudo unavailable or cancelled)";
            }
            *err = msg;
        }
        return false;
    }
    if (err)
        err->clear();
    return true;
}

} /* namespace reflash */
