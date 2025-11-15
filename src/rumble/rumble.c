// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

// Lightweight FF_RUMBLE effect manager (upload/erase/play) with GPIO backing.

#include "rumble.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "../gpio/gpio.h"

static void timespec_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static void timespec_add_ms(struct timespec *ts, unsigned int ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool timespec_after(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec > b->tv_sec) return true;
    if (a->tv_sec < b->tv_sec) return false;
    return a->tv_nsec >= b->tv_nsec;
}

void rumble_state_init(rumble_state_t *state)
{
    memset(state, 0, sizeof *state);
    state->gain = 0xFFFF;
}

static int allocate_slot(struct rumble_state *state)
{
    for (int i = 0; i < RUMBLE_MAX_EFFECTS; ++i) {
        if (!state->slots[i].in_use) {
            state->slots[i].in_use = true;
            return i;
        }
    }
    return -1;
}

int rumble_upload_effect(rumble_state_t *state, struct ff_effect *effect)
{
    if (!state || !effect) {
        errno = EINVAL;
        return -EINVAL;
    }

    if (effect->type != FF_RUMBLE) {
        errno = EINVAL;
        return -EINVAL;
    }

    int id = effect->id;
    if (id < 0) {
        id = allocate_slot(state);
        if (id < 0) {
            errno = ENOSPC;
            return -ENOSPC;
        }
    } else if (id >= RUMBLE_MAX_EFFECTS) {
        errno = EINVAL;
        return -EINVAL;
    } else if (!state->slots[id].in_use) {
        state->slots[id].in_use = true;
    }

    state->slots[id].effect = *effect;
    state->slots[id].effect.id = id;
    effect->id = id;
    return 0;
}

int rumble_erase_effect(rumble_state_t *state, int effect_id)
{
    if (!state) {
        errno = EINVAL;
        return -EINVAL;
    }
    if (effect_id < 0 || effect_id >= RUMBLE_MAX_EFFECTS) {
        errno = EINVAL;
        return -EINVAL;
    }
    state->slots[effect_id].in_use = false;
    if (state->rumble_active && state->slots[effect_id].effect.id == effect_id) {
        gpio_set_rumble(false);
        state->rumble_active = false;
    }
    return 0;
}

static void rumble_stop(struct rumble_state *state)
{
    if (!state->rumble_active) {
        return;
    }
    state->rumble_active = false;
    gpio_set_rumble(false);
}

void rumble_play_effect(rumble_state_t *state, int effect_id, int repeat)
{
    if (!state) return;
    if (effect_id < 0 || effect_id >= RUMBLE_MAX_EFFECTS) {
        return;
    }
    if (!state->slots[effect_id].in_use) {
        return;
    }

    struct ff_effect *effect = &state->slots[effect_id].effect;
    uint16_t mag = effect->u.rumble.strong_magnitude;
    if (effect->u.rumble.weak_magnitude > mag) {
        mag = effect->u.rumble.weak_magnitude;
    }

    mag = (uint32_t)mag * state->gain / 0xFFFF;
    if (mag == 0 || repeat == 0) {
        rumble_stop(state);
        return;
    }

    unsigned int reps = (repeat > 0) ? (unsigned int)repeat : 1;
    unsigned int duration_ms = effect->replay.length * reps;
    struct timespec now;
    timespec_now(&now);
    state->stop_time = now;
    timespec_add_ms(&state->stop_time, duration_ms);

    gpio_set_rumble(true);
    state->rumble_active = true;
}

void rumble_apply_gain(rumble_state_t *state, uint16_t gain)
{
    if (!state) return;
    state->gain = gain;
    if (!state->rumble_active) {
        return;
    }
    if (gain == 0) {
        rumble_stop(state);
    }
}

void rumble_tick(rumble_state_t *state)
{
    if (!state || !state->rumble_active) {
        return;
    }
    struct timespec now;
    timespec_now(&now);
    if (timespec_after(&now, &state->stop_time)) {
        rumble_stop(state);
    }
}
