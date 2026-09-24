/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
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

namespace reflash {

enum class CellStatus : std::uint8_t {
    Pending = 0,
    Ok = 1,       /* flushed to media (green) */
    ReadErr = 2,
    WriteErr = 3,
    Cached = 4,   /* written, still dirty/writeback (blue) */
};

enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

struct LogEntry {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::string message;
    CellStatus status = CellStatus::Ok;
    LogLevel level = LogLevel::Info;
    std::int64_t time_sec = 0;
};

struct ProgressSnapshot {
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_sec = 0.0;
    double eta_seconds = -1.0;
    bool paused = false;
    bool finished = false;
    bool failed = false;
    bool input_locked = false;
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
    /* Log line only. Does not paint a cell. */
    void message(LogLevel level, const std::string &msg);
    void set_paused(bool v);
    void set_finished(bool failed);
    /* During full-disk sync: UI should show wait cursor and ignore Pause/Stop. */
    void set_input_locked(bool v);
    bool input_locked() const;
    /* New run: same cell map, every cell pending, counters cleared. */
    void note_restart();
    /* After media sync: Cached cells become Ok (green). */
    void upgrade_cached_to_ok();
    size_t count_cells(CellStatus st) const;
    double elapsed_sec() const;
    ProgressSnapshot snapshot() const;
    std::vector<LogEntry> take_logs();
    std::vector<LogEntry> all_logs() const;
    std::uint64_t block_size() const { return block_size_; }
    std::uint64_t cell_size() const { return cell_size_; }
    unsigned cell_shift() const { return cell_shift_; }
    std::uint64_t bytes_done() const;
    std::uint64_t bytes_total() const;

private:
    mutable std::mutex mu_;
    std::uint64_t bytes_done_ = 0;
    std::uint64_t bytes_total_ = 0;
    std::uint64_t block_size_ = 4096;
    std::uint64_t cell_size_ = 4096;
    unsigned cell_shift_ = 0;
    double rate_ = 0.0;
    double eta_ = -1.0;
    double start_time_ = 0.0;
    bool paused_ = false;
    bool finished_ = false;
    bool failed_ = false;
    bool input_locked_ = false;
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

} /* namespace reflash */
