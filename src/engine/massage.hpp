/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "db/store.hpp"
#include "engine/pause.hpp"
#include "engine/progress.hpp"
#include "options.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace reflash {

class MassageEngine {
public:
    explicit MassageEngine(Options opts);
    ~MassageEngine();

    Progress &progress() { return progress_; }
    PauseControl &pause() { return pause_; }
    Store &store() { return store_; }
    const Options &options() const { return opts_; }
    void set_options(Options opts) { opts_ = std::move(opts); }

    bool start(std::string *err = nullptr);
    void join();
    bool running() const { return running_.load(); }
    int exit_code() const { return exit_code_; }

    /* Synchronous run (headless). */
    int run();

private:
    Options opts_;
    Progress progress_;
    PauseControl pause_;
    Store store_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    int exit_code_ = 1;

    int run_inner();
    bool prepare_db(std::string *err);
    std::uint64_t resolve_block_size(class Device &dev) const;
};

} /* namespace reflash */
