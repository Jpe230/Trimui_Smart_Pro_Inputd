#pragma once

#include "../common.h"

#define DEFAULT_DEADZONE 1024

int load_calibration_chain(const char *override_dir,
                           const char *primary_path,
                           const char *fallback_dir,
                           const char *filename,
                           joypad_cali_t *out);
