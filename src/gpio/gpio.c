#include "gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GPIO_LEFT_ENABLE 110  // PD14
#define GPIO_RIGHT_ENABLE 114 // PD18
#define GPIO_RUMBLE 227       // PH3
#define GPIO_DIP_SWITCH 243   // PH19
#define GPIO_5V_ENABLE 107    // PD11

static int write_file_str(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t len = (ssize_t)strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);
    return (written == len) ? 0 : -1;
}

static void gpio_write_value(int gpio, const char *node, const char *value)
{
    char path[128];
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/%s", gpio, node);
    if (write_file_str(path, value) != 0) {
        fprintf(stderr, "GPIO%d: failed to write %s (%s)\n", gpio, node, strerror(errno));
    }
}

static void gpio_export(int gpio)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", gpio);
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
        perror("open gpio export");
        return;
    }
    ssize_t len = (ssize_t)strlen(buf);
    if (write(fd, buf, len) < 0 && errno != EBUSY) {
        fprintf(stderr, "Failed to export GPIO%d: %s\n", gpio, strerror(errno));
    }
    close(fd);
}

static void init_gpio_output(int gpio, int value)
{
    gpio_export(gpio);
    gpio_write_value(gpio, "direction", "out");
    gpio_write_value(gpio, "value", value ? "1" : "0");
}

static void init_gpio_input(int gpio)
{
    gpio_export(gpio);
    gpio_write_value(gpio, "direction", "in");
}

void gpio_board_init(void)
{
    init_gpio_output(GPIO_LEFT_ENABLE, 1);
    init_gpio_output(GPIO_RIGHT_ENABLE, 1);
    init_gpio_output(GPIO_RUMBLE, 0);
    init_gpio_input(GPIO_DIP_SWITCH);

    gpio_export(GPIO_5V_ENABLE);
    gpio_write_value(GPIO_5V_ENABLE, "direction", "out");
    gpio_write_value(GPIO_5V_ENABLE, "value", "1");
}

void gpio_set_rumble(bool enable)
{
    static bool current_state = false;
    if (current_state == enable) {
        return;
    }
    current_state = enable;
    gpio_write_value(GPIO_RUMBLE, "value", enable ? "1" : "0");
}
