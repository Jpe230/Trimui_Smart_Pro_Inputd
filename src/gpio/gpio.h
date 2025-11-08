// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>

/**
 * Reproduce the stock inputd GPIO bring-up (power rails, DIP switch, rumble idle).
 */
void gpio_board_init(void);

/**
 * Drive the rumble GPIO high/low, suppressing redundant writes.
 */
void gpio_set_rumble(bool enable);
