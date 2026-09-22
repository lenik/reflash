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

namespace sdmsg {

enum class CellStatus : std::uint8_t {
    Pending = 0,
    Ok = 1,
    ReadErr = 2,
    WriteErr = 3,
};

struct LogEntry {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string message;
    CellStatus status = CellStatus::Ok;
};

struct ProgressSnapshot {
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_sec = 0.0;
    double eta_seconds = -1.0;
    bool paused = false;
    bool finished = false;
    bool failed = false;
    std::string phase;
    std::vector<CellStatus> cells;
    std::uint64_t cell_size = 0;
    unsigned cell_shift = 0;
};

class Progress {
public:
    void reset(std::uint64_t total_bytes, std::uint64_t block_size);
    void set_phase(const std::string &phase);
    void add_done(std::uint64_t n);
    void set_done(std::uint64_t n);
    void mark_cell(std::uint64_t offset, std::uint64_t length, CellStatus st);
    void log(std::uint64_t offset, std::uint64_t length, CellStatus st, const std::string &msg);
    void set_paused(bool v);
    void set_finished(bool failed);
    ProgressSnapshot snapshot() const;
    std::vector<LogEntry> take_logs();
    std::vector<LogEntry> all_logs() const;
    std::uint64_t block_size() const { return block_size_; }
    std::uint64_t cell_size() const { return cell_size_; }
    unsigned cell_shift() const { return cell_shift_; }

private:
    mutable std::mutex mu_;
    std::uint64_t bytes_done_ = 0;
    std::uint64_t bytes_total_ = 0;
    std::uint64_t block_size_ = 4096;
    std::uint64_t cell_size_ = 4096;
    unsigned cell_shift_ = 0;
    double rate_ = 0.0;
    double eta_ = -1.0;
    bool paused_ = false;
    bool finished_ = false;
    bool failed_ = false;
    std::string phase_;
    std::vector<CellStatus> cells_;
    std::vector<LogEntry> logs_;
    std::vector<LogEntry> pending_logs_;
    std::uint64_t last_bytes_ = 0;
    double last_time_ = 0.0;

    void recompute_rate_locked();
    void mark_cell_locked(std::uint64_t offset, std::uint64_t length, CellStatus st);
    static double now_sec();
};

} /* namespace sdmsg */
