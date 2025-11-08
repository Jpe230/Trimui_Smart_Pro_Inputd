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

static int create_uinput_device(controller_t *ctl)
{
    const uint16_t buttons[] = {
        BTN_TL, BTN_TL2, BTN_TR, BTN_TR2, BTN_DPAD_UP, BTN_DPAD_DOWN,
        BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_SOUTH, BTN_EAST, BTN_WEST,
        BTN_NORTH, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR
    };

    const uint16_t axes[] = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY
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

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof uidev);
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Trimui Smart Pro Virtual Pad");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1d6b;
    uidev.id.product = 0x1347;
    uidev.id.version = 1;
    uidev.ff_effects_max = RUMBLE_MAX_EFFECTS;

    uidev.absmin[ABS_X] = AXIS_MIN;
    uidev.absmax[ABS_X] = AXIS_MAX;
    uidev.absflat[ABS_X] = ctl->left.calibration.deadzone;
    uidev.absmin[ABS_Y] = AXIS_MIN;
    uidev.absmax[ABS_Y] = AXIS_MAX;
    uidev.absflat[ABS_Y] = ctl->left.calibration.deadzone;
    uidev.absmin[ABS_RX] = AXIS_MIN;
    uidev.absmax[ABS_RX] = AXIS_MAX;
    uidev.absflat[ABS_RX] = ctl->right.calibration.deadzone;
    uidev.absmin[ABS_RY] = AXIS_MIN;
    uidev.absmax[ABS_RY] = AXIS_MAX;
    uidev.absflat[ABS_RY] = ctl->right.calibration.deadzone;

    if (write(fd, &uidev, sizeof uidev) < 0) {
        perror("write uinput setup");
        close(fd);
        return -1;
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
    const struct {
        uint8_t mask;
        uint16_t code_left;
        uint16_t code_right;
    } mapping[] = {
        { 0x01u, BTN_TL, BTN_TR },
        { 0x02u, BTN_TL2, BTN_TR2 },
        { 0x04u, BTN_DPAD_UP, BTN_NORTH },
        { 0x20u, BTN_DPAD_DOWN, BTN_SOUTH },
        { 0x10u, BTN_DPAD_RIGHT, BTN_EAST },
        { 0x08u, BTN_DPAD_LEFT, BTN_WEST },
        { 0x40u, BTN_SELECT, BTN_START },
        { 0x80u, BTN_THUMBL, BTN_THUMBR },
    };

    joybutton_t prev = *last;
    if (prev.b == current.b) {
        return false;
    }

    bool dirty = false;
    for (size_t i = 0; i < sizeof mapping / sizeof mapping[0]; ++i) {
        bool prev_state = (prev.b & mapping[i].mask) != 0;
        bool curr_state = (current.b & mapping[i].mask) != 0;
        if (prev_state == curr_state) {
            continue;
        }
        uint16_t code = (side == SIDE_LEFT) ? mapping[i].code_left : mapping[i].code_right;
        emit_event(ufd, EV_KEY, code, curr_state ? 1 : 0);
        dirty = true;
    }

    *last = current;
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
                            false);
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
                            false);
        y = map_adc_to_axis(packet->y,
                            pad->calibration.y_min,
                            pad->calibration.y_max,
                            pad->calibration.y_zero,
                            pad->calibration.deadzone,
                            true);
        if (x != pad->last_x) {
            emit_event(ufd, EV_ABS, ABS_RX, x);
            pad->last_x = x;
            dirty = true;
        }
        if (y != pad->last_y) {
            emit_event(ufd, EV_ABS, ABS_RY, y);
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
    emit_event(ctl->uinput_fd, EV_ABS, ABS_RX, 0);
    emit_event(ctl->uinput_fd, EV_ABS, ABS_RY, 0);

    const uint16_t buttons[] = {
        BTN_TL, BTN_TL2, BTN_TR, BTN_TR2, BTN_DPAD_UP, BTN_DPAD_DOWN,
        BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_SOUTH, BTN_EAST, BTN_WEST,
        BTN_NORTH, BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR
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
        .uinput_fd = -1
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
                    sent_event |= (axis_dirty || btn_dirty);
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
