// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../common.h"

/**
 * Start the Trimui controller daemon until a termination signal is received.
 *
 * @param config_override_dir Optional directory checked first for calibration files.
 * @return process exit code (0 on clean shutdown, non-zero on fatal error).
 */
int run_controller(const char *config_override_dir);
