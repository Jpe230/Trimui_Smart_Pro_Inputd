// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "../common.h"

#define DEFAULT_DEADZONE 1024

/**
 * Load joystick calibration following the override → primary → fallback chain.
 *
 * @param override_dir Optional directory provided via CLI.
 * @param primary_path Path checked first (typically /mnt/UDISK/...).
 * @param fallback_dir Directory that contains the stock config files.
 * @param filename     Filename within override/fallback directories.
 * @param out          Destination struct to populate.
 * @return 0 on success, -1 if all sources failed (defaults already applied).
 */

int load_calibration_chain(const char *override_dir,
                           const char *primary_path,
                           const char *fallback_dir,
                           const char *filename,
                           joypad_cali_t *out);
