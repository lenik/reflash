/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "engine/pause.hpp"

#include <thread>
#include <chrono>

namespace reflash {

bool PauseControl::wait_if_paused() {
    while (paused_.load(std::memory_order_acquire)) {
        if (stop_.load(std::memory_order_acquire))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !stop_.load(std::memory_order_acquire);
}

} /* namespace reflash */
