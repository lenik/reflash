/*
 * Copyright (C) 2026 Lenik <reflash@bodz.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include <string>

namespace reflash {

/*
 * Fork a helper that stays in the host namespaces. Call once before entering
 * a user+mount namespace — pkexec breaks inside the userns.
 */
bool start_host_priv_agent();

bool host_priv_agent_running();

/* When true (GUI), never use sudo — only a graphical pkexec prompt. */
void set_host_priv_gui(bool gui);
bool host_priv_gui();

/* Run a shell script via the host agent (pkexec in GUI; pkexec then sudo headless). */
bool run_host_privileged_script(const std::string &script, std::string *err = nullptr);

} /* namespace reflash */
