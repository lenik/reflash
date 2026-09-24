/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "engine/progress.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

namespace sdmsg {

double Progress::now_sec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void Progress::reset(std::uint64_t total_bytes, std::uint64_t block_size) {
    std::lock_guard<std::mutex> lock(mu_);
    bytes_total_ = total_bytes;
    bytes_done_ = 0;
    block_size_ = block_size ? block_size : 4096;
    /* cell_size = block_size << n, choose n so cells in [256, 4096] */
    cell_shift_ = 0;
    std::uint64_t cells = total_bytes ? (total_bytes + block_size_ - 1) / block_size_ : 1;
    while (cells > 4096 && cell_shift_ < 40) {
        cell_shift_++;
        cells = (cells + 1) / 2;
    }
    while (cells < 256 && total_bytes > block_size_ && cell_shift_ > 0) {
        /* keep as-is if already minimal; only grow if we overshot by shrinking later — n grows only */
        break;
    }
    cell_size_ = block_size_ << cell_shift_;
    std::uint64_t ncells = total_bytes ? (total_bytes + cell_size_ - 1) / cell_size_ : 1;
    if (ncells == 0)
        ncells = 1;
    cells_.assign(static_cast<size_t>(ncells), CellStatus::Pending);
    rate_ = 0.0;
    eta_ = -1.0;
    paused_ = false;
    finished_ = false;
    failed_ = false;
    input_locked_ = false;
    phase_.clear();
    logs_.clear();
    pending_logs_.clear();
    last_bytes_ = 0;
    last_time_ = now_sec();
    start_time_ = last_time_;
}

void Progress::set_phase(const std::string &phase) {
    std::lock_guard<std::mutex> lock(mu_);
    phase_ = phase;
}

void Progress::recompute_rate_locked() {
    double t = now_sec();
    double dt = t - last_time_;
    if (dt >= 0.25) {
        double db = static_cast<double>(bytes_done_ - last_bytes_);
        double instant = db / dt;
        rate_ = rate_ > 0.0 ? (rate_ * 0.7 + instant * 0.3) : instant;
        last_bytes_ = bytes_done_;
        last_time_ = t;
        if (rate_ > 1.0 && bytes_done_ < bytes_total_)
            eta_ = static_cast<double>(bytes_total_ - bytes_done_) / rate_;
        else if (bytes_done_ >= bytes_total_)
            eta_ = 0.0;
        else
            eta_ = -1.0;
    }
}

void Progress::add_done(std::uint64_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    bytes_done_ += n;
    if (bytes_done_ > bytes_total_)
        bytes_done_ = bytes_total_;
    recompute_rate_locked();
}

void Progress::set_done(std::uint64_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    bytes_done_ = std::min(n, bytes_total_);
    recompute_rate_locked();
}

void Progress::mark_cell_locked(std::uint64_t offset, std::uint64_t length, CellStatus st) {
    if (cells_.empty() || cell_size_ == 0)
        return;
    std::uint64_t start = offset / cell_size_;
    std::uint64_t end = (offset + (length ? length : 1) - 1) / cell_size_;
    for (std::uint64_t i = start; i <= end && i < cells_.size(); ++i) {
        CellStatus cur = cells_[static_cast<size_t>(i)];
        if (st == CellStatus::ReadErr || st == CellStatus::WriteErr) {
            cells_[static_cast<size_t>(i)] = st;
        } else if (st == CellStatus::Ok) {
            /* Pending / Cached → Ok; never clear an error. */
            if (cur == CellStatus::Pending || cur == CellStatus::Cached || cur == CellStatus::Ok)
                cells_[static_cast<size_t>(i)] = CellStatus::Ok;
        } else if (st == CellStatus::Cached) {
            if (cur == CellStatus::Pending || cur == CellStatus::Cached)
                cells_[static_cast<size_t>(i)] = CellStatus::Cached;
        } else if (cur == CellStatus::Pending) {
            cells_[static_cast<size_t>(i)] = st;
        }
    }
}

void Progress::mark_cell(std::uint64_t offset, std::uint64_t length, CellStatus st) {
    std::lock_guard<std::mutex> lock(mu_);
    mark_cell_locked(offset, length, st);
}

void Progress::log(std::uint64_t offset, std::uint64_t length, CellStatus st,
                   const std::string &msg) {
    std::lock_guard<std::mutex> lock(mu_);
    LogEntry e;
    e.offset = offset;
    e.length = length;
    e.message = msg;
    e.status = st;
    e.time_sec = static_cast<std::int64_t>(std::time(nullptr));
    if (st == CellStatus::ReadErr || st == CellStatus::WriteErr)
        e.level = LogLevel::Error;
    else
        e.level = LogLevel::Info;
    logs_.push_back(e);
    pending_logs_.push_back(std::move(e));
    mark_cell_locked(offset, length, st);
}

void Progress::message(LogLevel level, const std::string &msg) {
    std::lock_guard<std::mutex> lock(mu_);
    LogEntry e;
    e.message = msg;
    e.level = level;
    e.status = CellStatus::Ok;
    e.time_sec = static_cast<std::int64_t>(std::time(nullptr));
    logs_.push_back(e);
    pending_logs_.push_back(std::move(e));
}

void Progress::set_paused(bool v) {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = v;
}

void Progress::note_restart() {
    std::lock_guard<std::mutex> lock(mu_);
    finished_ = false;
    failed_ = false;
    paused_ = false;
    input_locked_ = false;
    bytes_done_ = 0;
    rate_ = 0.0;
    eta_ = -1.0;
    phase_ = "prepare";
    start_time_ = now_sec();
    last_time_ = start_time_;
    last_bytes_ = 0;
    for (CellStatus &c : cells_)
        c = CellStatus::Pending;
}

void Progress::upgrade_cached_to_ok() {
    std::lock_guard<std::mutex> lock(mu_);
    for (CellStatus &c : cells_) {
        if (c == CellStatus::Cached)
            c = CellStatus::Ok;
    }
}

size_t Progress::count_cells(CellStatus st) const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = 0;
    for (CellStatus c : cells_) {
        if (c == st)
            ++n;
    }
    return n;
}

double Progress::elapsed_sec() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (start_time_ <= 0.0)
        return 0.0;
    return now_sec() - start_time_;
}

std::uint64_t Progress::bytes_done() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_done_;
}

std::uint64_t Progress::bytes_total() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_total_;
}

void Progress::set_finished(bool failed) {
    std::lock_guard<std::mutex> lock(mu_);
    finished_ = true;
    failed_ = failed;
    input_locked_ = false;
    if (!failed)
        bytes_done_ = bytes_total_;
}

void Progress::set_input_locked(bool v) {
    std::lock_guard<std::mutex> lock(mu_);
    input_locked_ = v;
}

bool Progress::input_locked() const {
    std::lock_guard<std::mutex> lock(mu_);
    return input_locked_;
}

ProgressSnapshot Progress::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    ProgressSnapshot s;
    s.bytes_done = bytes_done_;
    s.bytes_total = bytes_total_;
    s.bytes_per_sec = rate_;
    s.eta_seconds = eta_;
    s.paused = paused_;
    s.finished = finished_;
    s.failed = failed_;
    s.input_locked = input_locked_;
    s.phase = phase_;
    s.cells = cells_;
    s.cell_size = cell_size_;
    s.cell_shift = cell_shift_;
    return s;
}

std::vector<LogEntry> Progress::take_logs() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LogEntry> out;
    out.swap(pending_logs_);
    return out;
}

std::vector<LogEntry> Progress::all_logs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return logs_;
}

} /* namespace sdmsg */
