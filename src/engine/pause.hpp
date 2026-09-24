/*
 * Copyright (C) 2026 Lenik <sdmsg@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <atomic>

namespace sdmsg {

class PauseControl {
public:
    void pause() { paused_.store(true, std::memory_order_release); }
    void resume() { paused_.store(false, std::memory_order_release); }
    void request_stop() { stop_.store(true, std::memory_order_release); }
    void reset() {
        paused_.store(false, std::memory_order_release);
        stop_.store(false, std::memory_order_release);
    }
    bool stop_requested() const { return stop_.load(std::memory_order_acquire); }
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }

    /* Call only at block boundaries. Blocks while paused unless stop. */
    bool wait_if_paused();

private:
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
};

} /* namespace sdmsg */
