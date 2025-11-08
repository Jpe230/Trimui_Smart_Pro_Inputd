# Trimui Smart Pro Input Daemon

Userspace daemon that bonds the Trimui Smart Pro’s two serial pads into a single Linux joystick (`/dev/input/js0`). It reads both halves over `/dev/ttyS4` and `/dev/ttyS3`, applies the on-device calibration files, exposes a virtual controller via `uinput`, and mirrors the stock firmware’s GPIO bring-up so it can run immediately after boot.

## Highlights

- **Dual-serial aggregation:** Continuously polls both pad MCUs at 1 kHz, reopens the TTY automatically when errors occur, and keeps axis/button state in sync with the uinput device.
- **Calibration aware:** Loads `joypad.config` and `joypad_right.config` (left/right) from `/mnt/UDISK/`, falling back to `/userdata/system/config/trimui-input/`, with an optional override directory passed on the command line. Each file can specify `x_min`, `x_max`, `x_zero`, `y_min`, `y_max`, `y_zero`, and `deadzone` (default 1024).
- **Rumble support:** Advertises `FF_RUMBLE`/`FF_GAIN`, keeps a small effect pool, and translates play commands into GPIO 227 toggles so native ports can vibrate the device.
- **Board bring-up:** Reproduces the stock `inputd` GPIO pokes (PD14/PD18 rails, rumble default, DIP input, optional 5 V enable) so the pads, DIP switch, and rumble motor are usable even on a cold boot.
- **Deterministic startup:** After the uinput node is created the daemon waits 1 s before zeroing the sticks to match the OEM behavior and reduce drift.

## Building

The repo ships with a cross-compilation container. From the project root:

```bash
docker compose up --build
```

The resulting binary lives at `build/tsp_inputd/bin/tsp_inputd`.

If you prefer a native toolchain, install `gcc`, `make`, and standard headers for your aarch64 rootfs, then run:

```bash
make
```

## Running

```bash
./build/tsp_inputd/bin/tsp_inputd [config_dir]
```

- Without arguments the daemon searches `/mnt/UDISK` first, then `/userdata/system/config/trimui-input/`.
- If `config_dir` is supplied, the daemon looks for `joypad.config` and `joypad_right.config` there before falling back to the default locations.
- All GPIO control happens via sysfs; run as root (or grant sufficient permissions) so the daemon can drive the pins and open `/dev/uinput`.

## Configuration File Format

```
x_min=0
x_max=4095
y_min=0
y_max=4095
x_zero=2048
y_zero=2048
deadzone=1024
```

Values are unsigned integers. `deadzone` clamps the ABS flat value and software filtering range; if omitted it defaults to 1024.

## Notes

- The daemon targets the stock Trimui Smart Pro kernel (19200 baud serial pads, sysfs GPIO numbers shown above). If your board revision changes pin muxing, update `src/gpio/gpio.c`.
- Rumble currently uses an on/off duty cycle. If you need variable intensity, consider swapping GPIO 227 to a PWM-capable interface or extend the driver with a software PWM loop.
- Calibration files are not modified by the daemon; use the OEM calibration utility or your own tool to update them, then restart this service.
