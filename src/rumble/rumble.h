// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <linux/input.h>
#include <stdint.h>
#include <stdbool.h>

#include <time.h>

#define RUMBLE_MAX_EFFECTS 8

typedef struct {
    struct ff_effect effect;
    bool in_use;
} rumble_slot_t;

/**
 * Tracks uploaded rumble effects and the currently playing one.
 */
typedef struct rumble_state {
    rumble_slot_t slots[RUMBLE_MAX_EFFECTS];
    bool rumble_active;
    struct timespec stop_time;
    uint16_t gain;
} rumble_state_t;

/**
 * Initialize a rumble_state instance with no uploaded effects and max gain.
 *
 * @param state Rumble container to initialize.
 */
void rumble_state_init(rumble_state_t *state);

/**
 * Upload or replace an FF_RUMBLE effect in the local slot pool.
 *
 * @param state  Rumble container to mutate.
 * @param effect Effect payload from UI_BEGIN_FF_UPLOAD; updated with slot id.
 * @return 0 on success, -1 if the slot pool is full or the payload is invalid.
 */
int rumble_upload_effect(rumble_state_t *state, struct ff_effect *effect);

/**
 * Erase a previously uploaded effect slot.
 *
 * @param state     Rumble container to mutate.
 * @param effect_id Slot index reported by upload.
 * @return 0 on success, -1 if the slot does not exist.
 */
int rumble_erase_effect(rumble_state_t *state, int effect_id);

/**
 * Start or stop playback of the selected effect.
 *
 * @param state     Rumble container to mutate.
 * @param effect_id Slot index to play.
 * @param repeat    Number of iterations requested by uinput (0 stops playback).
 */
void rumble_play_effect(rumble_state_t *state, int effect_id, int repeat);

/**
 * Update the global rumble gain as provided by the host OS.
 *
 * @param state Rumble container to mutate.
 * @param gain  0-0xFFFF scaling factor applied to the stored magnitudes.
 */
void rumble_apply_gain(rumble_state_t *state, uint16_t gain);

/**
 * Periodic timer hook to turn the motor off when the requested duration expires.
 *
 * @param state Rumble container to service.
 */
void rumble_tick(rumble_state_t *state);
