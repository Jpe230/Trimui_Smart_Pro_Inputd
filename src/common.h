// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <inttypes.h>

/**
 * Baud rate for the serial devices
 */
#define BAUD_RATE B19200

/**
 * Struct for the state of each button bit.
 * (Use JOYBTN_* names to avoid clashes with linux/input.h macros.)
 *
 * Note: JOYBTN_F1 is unused in the left side
 */
typedef struct {
    uint8_t JOYBTN_BUMPER : 1;
    uint8_t JOYBTN_TRIGGER : 1;
    uint8_t JOYBTN_NORTH : 1;
    uint8_t JOYBTN_WEST : 1;
    uint8_t JOYBTN_EAST : 1;
    uint8_t JOYBTN_SOUTH : 1;
    uint8_t JOYBTN_F1 : 1;
    uint8_t JOYBTN_F2 : 1;
} joybutton_bitfield_t;

/**
 * Union for the bitfield
 */
typedef union
{
	uint8_t b;
	joybutton_bitfield_t bf;
} joybutton_t;
 
/**
 * Main Struct for the serial message:
 *
 * Header: Unknown meaning
 * Buttons: The bitfield for the button state
 * X & Y : The current ADC values
 */
typedef struct {
    uint16_t header;
    joybutton_t buttons;
    uint16_t x;
    uint16_t y;
} joypad_struct_t;

/**
 * Struct for the calibration data
 */
typedef struct {
    uint16_t x_min;
    uint16_t x_max;
    uint16_t y_min;
    uint16_t y_max;
    uint16_t x_zero;
    uint16_t y_zero;
    uint16_t deadzone;
} joypad_cali_t;
