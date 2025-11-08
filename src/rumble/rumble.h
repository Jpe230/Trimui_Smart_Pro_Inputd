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

typedef struct rumble_state {
    rumble_slot_t slots[RUMBLE_MAX_EFFECTS];
    bool rumble_active;
    struct timespec stop_time;
    uint16_t gain;
} rumble_state_t;

void rumble_state_init(rumble_state_t *state);
int rumble_upload_effect(rumble_state_t *state, struct ff_effect *effect);
int rumble_erase_effect(rumble_state_t *state, int effect_id);
void rumble_play_effect(rumble_state_t *state, int effect_id, int repeat);
void rumble_apply_gain(rumble_state_t *state, uint16_t gain);
void rumble_tick(rumble_state_t *state);
