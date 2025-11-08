#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) {
        ++s;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    *end = '\0';
    return s;
}

static void set_default_calibration(joypad_cali_t *c)
{
    c->x_min = 0;
    c->x_max = 4095;
    c->y_min = 0;
    c->y_max = 4095;
    c->x_zero = 2048;
    c->y_zero = 2048;
    c->deadzone = DEFAULT_DEADZONE;
}

static bool parse_calibration_line(joypad_cali_t *cali, const char *key, const char *value)
{
    char *endptr = NULL;
    unsigned long val = strtoul(value, &endptr, 10);
    if (!value || *value == '\0' || (endptr && *endptr != '\0')) {
        return false;
    }

    if (strcmp(key, "x_min") == 0) {
        cali->x_min = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "x_max") == 0) {
        cali->x_max = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "y_min") == 0) {
        cali->y_min = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "y_max") == 0) {
        cali->y_max = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "x_zero") == 0) {
        cali->x_zero = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "y_zero") == 0) {
        cali->y_zero = (uint16_t)val;
        return true;
    }
    if (strcmp(key, "deadzone") == 0) {
        cali->deadzone = (uint16_t)val;
        return true;
    }
    return false;
}

static int load_calibration_from_file(const char *path, joypad_cali_t *cali)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char line[160];
    bool parsed = false;
    while (fgets(line, sizeof line, f)) {
        char *trimmed = trim(line);
        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }
        char *eq = strchr(trimmed, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *key = trim(trimmed);
        const char *value = trim(eq + 1);
        if (parse_calibration_line(cali, key, value)) {
            parsed = true;
        }
    }

    fclose(f);
    return parsed ? 0 : -1;
}

int load_calibration_chain(const char *override_dir,
                           const char *primary_path,
                           const char *fallback_dir,
                           const char *filename,
                           joypad_cali_t *out)
{
    set_default_calibration(out);

    if (override_dir && *override_dir) {
        char override_path[PATH_MAX];
        snprintf(override_path, sizeof override_path, "%s/%s", override_dir, filename);
        if (load_calibration_from_file(override_path, out) == 0) {
            fprintf(stderr, "Loaded calibration from %s\n", override_path);
            return 0;
        }
    }

    if (load_calibration_from_file(primary_path, out) == 0) {
        fprintf(stderr, "Loaded calibration from %s\n", primary_path);
        return 0;
    }

    char fallback_path[PATH_MAX];
    snprintf(fallback_path, sizeof fallback_path, "%s/%s", fallback_dir, filename);
    if (load_calibration_from_file(fallback_path, out) == 0) {
        fprintf(stderr, "Loaded calibration from %s\n", fallback_path);
        return 0;
    }

    fprintf(stderr, "Using default calibration for %s (files missing)\n", filename);
    return -1;
}
