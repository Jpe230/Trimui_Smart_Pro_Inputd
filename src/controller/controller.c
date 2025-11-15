// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

// Core runtime: reads both serial pads, maps them to uinput, and handles FF rumble.

#include "controller.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../config/config.h"
#include "../gpio/gpio.h"
#include "../rumble/rumble.h"
#include "../serial/serial-joystick.h"

#define LEFT_SERIAL_PORT "/dev/ttyS4"
#define RIGHT_SERIAL_PORT "/dev/ttyS3"

#define LEFT_CONFIG_PRIMARY "/mnt/UDISK/joypad.config"
#define RIGHT_CONFIG_PRIMARY "/mnt/UDISK/joypad_right.config"
#define CONFIG_FALLBACK_DIR "/userdata/system/config/trimui-input"
#define LEFT_CONFIG_NAME "joypad.config"
#define RIGHT_CONFIG_NAME "joypad_right.config"

#define AXIS_MIN (-32768)
#define AXIS_MAX (32767)

// Identifies which half-pad produced a packet (left or right).
typedef enum {
    SIDE_LEFT = 0,
    SIDE_RIGHT = 1
} joystick_side_t;

// State for one serial pad half (file descriptor, calibration, last values).
typedef struct {
    const char *serial_path;
    const char *primary_cfg;
    const char *fallback_name;
    joypad_cali_t calibration;
    joybutton_t last_buttons;
    int16_t last_x;
    int16_t last_y;
    int fd;
} halfpad_t;

// Aggregated controller composed of both halves plus the uinput + rumble handles.
typedef struct {
    halfpad_t left;
    halfpad_t right;
    int uinput_fd;
    rumble_state_t rumble;
    int8_t hat_x;
    int8_t hat_y;
} controller_t;

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

static inline int16_t clamp_axis(int value)
{
    if (value > AXIS_MAX) return AXIS_MAX;
    if (value < AXIS_MIN) return AXIS_MIN;
    return (int16_t)value;
}

static inline int fast_round(double value)
{
    return (int)((value >= 0.0) ? (value + 0.5) : (value - 0.5));
}

static int16_t map_adc_to_axis(uint16_t raw,
                               uint16_t min,
                               uint16_t max,
                               uint16_t zero,
                               uint16_t deadzone,
                               bool invert)
{
    int32_t centered = (int32_t)raw - (int32_t)zero;
    int32_t range = (centered >= 0)
                        ? (int32_t)max - (int32_t)zero
                        : (int32_t)zero - (int32_t)min;

    if (range == 0) {
        return 0;
    }

    double normalized = (double)centered / (double)range;
    if (normalized > 1.0) normalized = 1.0;
    if (normalized < -1.0) normalized = -1.0;

    double scaled = normalized * (normalized >= 0.0 ? AXIS_MAX : -AXIS_MIN);
    int value = fast_round(scaled);
    if (invert) {
        value = -value;
    }

    int dz = deadzone;
    if (dz <= 0) dz = DEFAULT_DEADZONE;
    if (dz > AXIS_MAX) dz = AXIS_MAX;
    if (abs(value) < dz) {
        value = 0;
    }
    return clamp_axis(value);
}

static int emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) < 0) {
        perror("write uinput");
        return -1;
    }
    return 0;
}

static int sync_events(int fd)
{
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int configure_abs_axis(int fd, uint16_t code, int min, int max, int flat)
{
    struct uinput_abs_setup abs = {
        .code = code,
        .absinfo = {
            .minimum = min,
            .maximum = max,
            .flat = flat
        }
    };
    return ioctl(fd, UI_ABS_SETUP, &abs);
}

static int create_uinput_device(controller_t *ctl)
{
    const uint16_t buttons[] = {
        BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE
    };

    const uint16_t axes[] = {
        ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
    };

    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) == -1 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) == -1 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) == -1 ||
        ioctl(fd, UI_SET_EVBIT, EV_FF) == -1) {
        perror("ioctl UI_SET_EVBIT");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) == -1 ||
        ioctl(fd, UI_SET_FFBIT, FF_GAIN) == -1) {
        perror("ioctl UI_SET_FFBIT");
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        if (ioctl(fd, UI_SET_KEYBIT, buttons[i]) == -1) {
            perror("ioctl UI_SET_KEYBIT");
            close(fd);
            return -1;
        }
    }

    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; ++i) {
        if (ioctl(fd, UI_SET_ABSBIT, axes[i]) == -1) {
            perror("ioctl UI_SET_ABSBIT");
            close(fd);
            return -1;
        }
    }

    bool new_setup = true;
    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x0000;
    setup.id.product = 0x0000;
    setup.id.version = 1;
    setup.ff_effects_max = RUMBLE_MAX_EFFECTS;
    snprintf(setup.name, sizeof setup.name, "TRIMUI Smart Pro Controller");
    if (ioctl(fd, UI_DEV_SETUP, &setup) == -1) {
        if (errno != EINVAL) {
            perror("ioctl UI_DEV_SETUP");
            close(fd);
            return -1;
        }
        new_setup = false;
    }

    if (new_setup) {
        if (configure_abs_axis(fd, ABS_X, AXIS_MIN, AXIS_MAX, ctl->left.calibration.deadzone) == -1 ||
            configure_abs_axis(fd, ABS_Y, AXIS_MIN, AXIS_MAX, ctl->left.calibration.deadzone) == -1 ||
            configure_abs_axis(fd, ABS_Z, AXIS_MIN, AXIS_MAX, ctl->right.calibration.deadzone) == -1 ||
            configure_abs_axis(fd, ABS_RZ, AXIS_MIN, AXIS_MAX, ctl->right.calibration.deadzone) == -1 ||
            configure_abs_axis(fd, ABS_HAT0X, -1, 1, 0) == -1 ||
            configure_abs_axis(fd, ABS_HAT0Y, -1, 1, 0) == -1) {
            perror("ioctl UI_ABS_SETUP");
            close(fd);
            return -1;
        }
    } else {
        struct uinput_user_dev legacy;
        memset(&legacy, 0, sizeof legacy);
        snprintf(legacy.name, sizeof legacy.name, "TRIMUI Smart Pro Controller");
        legacy.id.bustype = BUS_USB;
        legacy.id.vendor = 0x0000;
        legacy.id.product = 0x0000;
        legacy.id.version = 1;
        legacy.ff_effects_max = RUMBLE_MAX_EFFECTS;
        legacy.absmin[ABS_X] = AXIS_MIN;
        legacy.absmax[ABS_X] = AXIS_MAX;
        legacy.absflat[ABS_X] = ctl->left.calibration.deadzone;
        legacy.absmin[ABS_Y] = AXIS_MIN;
        legacy.absmax[ABS_Y] = AXIS_MAX;
        legacy.absflat[ABS_Y] = ctl->left.calibration.deadzone;
        legacy.absmin[ABS_Z] = AXIS_MIN;
        legacy.absmax[ABS_Z] = AXIS_MAX;
        legacy.absflat[ABS_Z] = ctl->right.calibration.deadzone;
        legacy.absmin[ABS_RZ] = AXIS_MIN;
        legacy.absmax[ABS_RZ] = AXIS_MAX;
        legacy.absflat[ABS_RZ] = ctl->right.calibration.deadzone;
        legacy.absmin[ABS_HAT0X] = -1;
        legacy.absmax[ABS_HAT0X] = 1;
        legacy.absmin[ABS_HAT0Y] = -1;
        legacy.absmax[ABS_HAT0Y] = 1;
        if (write(fd, &legacy, sizeof legacy) < 0) {
            perror("write uinput setup");
            close(fd);
            return -1;
        }
    }

    if (ioctl(fd, UI_DEV_CREATE) == -1) {
        perror("ioctl UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    usleep(1000000); // allow extra time for sticks to settle before zeroing
    return fd;
}

static void destroy_uinput_device(int fd)
{
    if (fd < 0) return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}

static int reopen_serial(halfpad_t *pad)
{
    if (pad->fd >= 0) {
        closeSerialJoystick(pad->fd);
        pad->fd = -1;
    }

    pad->fd = openSerialJoystick(pad->serial_path);
    if (pad->fd < 0) {
        fprintf(stderr, "Failed to open %s\n", pad->serial_path);
    } else {
        fprintf(stdout, "Opened %s\n", pad->serial_path);
    }
    return pad->fd;
}

static bool update_buttons(int ufd, joystick_side_t side, joybutton_t *last, joybutton_t current)
{
    typedef struct {
        uint8_t mask;
        uint16_t code;
    } button_map_entry_t;

    static const button_map_entry_t left_map[] = {
        { 0x01u, BTN_TL },     // L1
        { 0x02u, BTN_TL2 },    // L2
        { 0x80u, BTN_MODE },   // Menu/Home button
    };

    static const button_map_entry_t right_map[] = {
        { 0x10u, BTN_SOUTH },  // B
        { 0x20u, BTN_EAST },   // A
        { 0x04u, BTN_NORTH },  // Y
        { 0x08u, BTN_WEST },   // X
        { 0x01u, BTN_TR },     // R1
        { 0x02u, BTN_TR2 },    // R2
        { 0x40u, BTN_SELECT }, // Select
        { 0x80u, BTN_START },  // Start
    };

    const button_map_entry_t *map = (side == SIDE_LEFT) ? left_map : right_map;
    const size_t map_len = (side == SIDE_LEFT)
                               ? (sizeof left_map / sizeof left_map[0])
                               : (sizeof right_map / sizeof right_map[0]);

    joybutton_t prev = *last;
    if (prev.b == current.b) {
        return false;
    }

    bool dirty = false;
    for (size_t i = 0; i < map_len; ++i) {
        bool prev_state = (prev.b & map[i].mask) != 0;
        bool curr_state = (current.b & map[i].mask) != 0;
        if (prev_state == curr_state) {
            continue;
        }
        emit_event(ufd, EV_KEY, map[i].code, curr_state ? 1 : 0);
        dirty = true;
    }

    *last = current;
    return dirty;
}

static bool update_hat(controller_t *ctl, joybutton_t buttons)
{
    int8_t new_x = 0;
    if (buttons.b & 0x08u) {
        new_x = -1;
    } else if (buttons.b & 0x10u) {
        new_x = 1;
    }

    int8_t new_y = 0;
    if (buttons.b & 0x04u) {
        new_y = -1;
    } else if (buttons.b & 0x20u) {
        new_y = 1;
    }

    bool dirty = false;
    if (new_x != ctl->hat_x) {
        emit_event(ctl->uinput_fd, EV_ABS, ABS_HAT0X, new_x);
        ctl->hat_x = new_x;
        dirty = true;
    }
    if (new_y != ctl->hat_y) {
        emit_event(ctl->uinput_fd, EV_ABS, ABS_HAT0Y, new_y);
        ctl->hat_y = new_y;
        dirty = true;
    }

    return dirty;
}

static bool update_axes(int ufd, joystick_side_t side, halfpad_t *pad, const joypad_struct_t *packet)
{
    bool dirty = false;
    int16_t x, y;
    if (side == SIDE_LEFT) {
        x = map_adc_to_axis(packet->x,
                            pad->calibration.x_min,
                            pad->calibration.x_max,
                            pad->calibration.x_zero,
                            pad->calibration.deadzone,
                            true);
        y = map_adc_to_axis(packet->y,
                            pad->calibration.y_min,
                            pad->calibration.y_max,
                            pad->calibration.y_zero,
                            pad->calibration.deadzone,
                            true);
        if (x != pad->last_x) {
            emit_event(ufd, EV_ABS, ABS_X, x);
            pad->last_x = x;
            dirty = true;
        }
        if (y != pad->last_y) {
            emit_event(ufd, EV_ABS, ABS_Y, y);
            pad->last_y = y;
            dirty = true;
        }
    } else {
        x = map_adc_to_axis(packet->x,
                            pad->calibration.x_min,
                            pad->calibration.x_max,
                            pad->calibration.x_zero,
                            pad->calibration.deadzone,
                            true);
        y = map_adc_to_axis(packet->y,
                            pad->calibration.y_min,
                            pad->calibration.y_max,
                            pad->calibration.y_zero,
                            pad->calibration.deadzone,
                            true);
        if (x != pad->last_x) {
            emit_event(ufd, EV_ABS, ABS_Z, x);
            pad->last_x = x;
            dirty = true;
        }
        if (y != pad->last_y) {
            emit_event(ufd, EV_ABS, ABS_RZ, y);
            pad->last_y = y;
            dirty = true;
        }
    }
    return dirty;
}

static void prime_state(controller_t *ctl)
{
    emit_event(ctl->uinput_fd, EV_ABS, ABS_X, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_Y, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_Z, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_RZ, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_HAT0X, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_HAT0Y, 0);
    ctl->hat_x = 0;
    ctl->hat_y = 0;

    const uint16_t buttons[] = {
        BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST,
        BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE
    };
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        emit_event(ctl->uinput_fd, EV_KEY, buttons[i], 0);
    }
    sync_events(ctl->uinput_fd);
}

static void process_ff_upload(controller_t *ctl)
{
    struct uinput_ff_upload upload;
    memset(&upload, 0, sizeof upload);
    if (ioctl(ctl->uinput_fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) {
        perror("UI_BEGIN_FF_UPLOAD");
        return;
    }

    upload.retval = rumble_upload_effect(&ctl->rumble, &upload.effect);
    if (upload.retval != 0) {
        fprintf(stderr, "Failed to upload rumble effect\n");
    }

    if (ioctl(ctl->uinput_fd, UI_END_FF_UPLOAD, &upload) < 0) {
        perror("UI_END_FF_UPLOAD");
    }
}

static void process_ff_erase(controller_t *ctl)
{
    struct uinput_ff_erase erase;
    memset(&erase, 0, sizeof erase);
    if (ioctl(ctl->uinput_fd, UI_BEGIN_FF_ERASE, &erase) < 0) {
        perror("UI_BEGIN_FF_ERASE");
        return;
    }

    erase.retval = rumble_erase_effect(&ctl->rumble, erase.effect_id);
    if (erase.retval != 0) {
        fprintf(stderr, "Failed to erase rumble effect %d\n", erase.effect_id);
    }

    if (ioctl(ctl->uinput_fd, UI_END_FF_ERASE, &erase) < 0) {
        perror("UI_END_FF_ERASE");
    }
}

static void process_uinput_events(controller_t *ctl)
{
    struct input_event ev;
    while (true) {
        ssize_t r = read(ctl->uinput_fd, &ev, sizeof ev);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            perror("read uinput");
            break;
        }
        if (r != sizeof ev) {
            break;
        }

        if (ev.type == EV_UINPUT) {
            if (ev.code == UI_FF_UPLOAD) {
                process_ff_upload(ctl);
            } else if (ev.code == UI_FF_ERASE) {
                process_ff_erase(ctl);
            }
            continue;
        }

        if (ev.type == EV_FF) {
            if (ev.code == FF_GAIN) {
                rumble_apply_gain(&ctl->rumble, (uint16_t)ev.value);
            } else {
                rumble_play_effect(&ctl->rumble, ev.code, ev.value);
            }
        }
    }
}

int run_controller(const char *config_override_dir)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    controller_t ctl = {
        .left = {
            .serial_path = LEFT_SERIAL_PORT,
            .primary_cfg = LEFT_CONFIG_PRIMARY,
            .fallback_name = LEFT_CONFIG_NAME,
            .last_buttons = { .b = 0 },
            .last_x = 0,
            .last_y = 0,
            .fd = -1,
        },
        .right = {
            .serial_path = RIGHT_SERIAL_PORT,
            .primary_cfg = RIGHT_CONFIG_PRIMARY,
            .fallback_name = RIGHT_CONFIG_NAME,
            .last_buttons = { .b = 0 },
            .last_x = 0,
            .last_y = 0,
            .fd = -1,
        },
        .uinput_fd = -1,
        .hat_x = 0,
        .hat_y = 0
    };
    rumble_state_init(&ctl.rumble);

    load_calibration_chain(config_override_dir, ctl.left.primary_cfg, CONFIG_FALLBACK_DIR,
                           ctl.left.fallback_name, &ctl.left.calibration);
    load_calibration_chain(config_override_dir, ctl.right.primary_cfg, CONFIG_FALLBACK_DIR,
                           ctl.right.fallback_name, &ctl.right.calibration);

    gpio_board_init();

    if (reopen_serial(&ctl.left) < 0 || reopen_serial(&ctl.right) < 0) {
        fprintf(stderr, "Unable to open serial devices\n");
        return EXIT_FAILURE;
    }

    ctl.uinput_fd = create_uinput_device(&ctl);
    if (ctl.uinput_fd < 0) {
        closeSerialJoystick(ctl.left.fd);
        closeSerialJoystick(ctl.right.fd);
        return EXIT_FAILURE;
    }

    prime_state(&ctl);

    struct pollfd pfds[3];
    const int poll_timeout_ms = 1;
    while (keep_running) {
        pfds[0].fd = ctl.left.fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = ctl.right.fd;
        pfds[1].events = POLLIN;
        pfds[2].fd = ctl.uinput_fd;
        pfds[2].events = POLLIN;

        int ret = poll(pfds, 3, poll_timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        bool sent_event = false;
        if (pfds[0].revents & POLLIN) {
            joypad_struct_t sample;
            int read_res;
            do {
                read_res = readSerialJoypad(ctl.left.fd, &sample);
                if (read_res == 1) {
                    bool axis_dirty = update_axes(ctl.uinput_fd, SIDE_LEFT, &ctl.left, &sample);
                    bool btn_dirty = update_buttons(ctl.uinput_fd, SIDE_LEFT, &ctl.left.last_buttons, sample.buttons);
                    bool hat_dirty = update_hat(&ctl, sample.buttons);
                    sent_event |= (axis_dirty || btn_dirty || hat_dirty);
                } else if (read_res < 0) {
                    fprintf(stderr, "Left serial read error, trying to reopen...\n");
                    reopen_serial(&ctl.left);
                    break;
                }
            } while (read_res == 1);
        }

        if (pfds[1].revents & POLLIN) {
            joypad_struct_t sample;
            int read_res;
            do {
                read_res = readSerialJoypad(ctl.right.fd, &sample);
                if (read_res == 1) {
                    bool axis_dirty = update_axes(ctl.uinput_fd, SIDE_RIGHT, &ctl.right, &sample);
                    bool btn_dirty = update_buttons(ctl.uinput_fd, SIDE_RIGHT, &ctl.right.last_buttons, sample.buttons);
                    sent_event |= (axis_dirty || btn_dirty);
                } else if (read_res < 0) {
                    fprintf(stderr, "Right serial read error, trying to reopen...\n");
                    reopen_serial(&ctl.right);
                    break;
                }
            } while (read_res == 1);
        }

        if (pfds[2].revents & POLLIN) {
            process_uinput_events(&ctl);
        }

        rumble_tick(&ctl.rumble);

        if (sent_event) {
            sync_events(ctl.uinput_fd);
        }
    }

    destroy_uinput_device(ctl.uinput_fd);
    closeSerialJoystick(ctl.left.fd);
    closeSerialJoystick(ctl.right.fd);
    gpio_set_rumble(false);
    return EXIT_SUCCESS;
}
