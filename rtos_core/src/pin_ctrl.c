/*
 * GPIO control for this board's control-board relay outputs (RO0-RO3)
 * and the ASIC chain reset line, via the RT-Smart /dev/gpio character
 * device.
 */

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "pin_ctrl.h"

/* ioctl command numbers: 'G'-magic ioctls on /dev/gpio, each taking a
 * {pin, value} pair. */
#define KD_IOCTL_SET_OUTPUT  _IOW('G', 0, int)
#define KD_IOCTL_SET_INPUT   _IOW('G', 1, int)
#define KD_IOCTL_WRITE_LOW   _IOW('G', 4, int)
#define KD_IOCTL_WRITE_HIGH  _IOW('G', 5, int)
#define KD_IOCTL_READ_VALUE  _IOW('G', 12, int)

typedef struct {
        unsigned short pin;
        unsigned short value;
} gpio_ioctl_arg_t;

/* Control-board relay outputs (RO0-RO3), by GPIO number. */
static const int relay_pins[4] = { 2, 3, 4, 5 };

static int gpio_dev_fd = -1;

void gpio_init(uint8_t cnt)
{
        gpio_ioctl_arg_t arg;
        int i;

        gpio_dev_fd = open("/dev/gpio", O_RDWR);
        if (gpio_dev_fd < 0) {
                return;
        }

        for (i = 0; i < cnt; i++) {
                arg.pin = relay_pins[i];
                ioctl(gpio_dev_fd, KD_IOCTL_SET_OUTPUT, &arg);
        }
}

void gpio_set_value(uint8_t m, uint8_t value)
{
        gpio_ioctl_arg_t arg;

        arg.pin = relay_pins[m];
        ioctl(gpio_dev_fd, value ? KD_IOCTL_WRITE_HIGH : KD_IOCTL_WRITE_LOW, &arg);
}

int gpio_asic_rst_init(void)
{
        gpio_ioctl_arg_t arg;

        if (gpio_dev_fd < 0)
                return -1;

        arg.pin = ASIC_RST_PIN;
        return ioctl(gpio_dev_fd, KD_IOCTL_SET_OUTPUT, &arg);
}

void gpio_asic_rst_assert(void)
{
        gpio_ioctl_arg_t arg;

        arg.pin = ASIC_RST_PIN;
        ioctl(gpio_dev_fd, KD_IOCTL_WRITE_HIGH, &arg);
}

void gpio_asic_rst_deassert(void)
{
        gpio_ioctl_arg_t arg;

        arg.pin = ASIC_RST_PIN;
        ioctl(gpio_dev_fd, KD_IOCTL_WRITE_LOW, &arg);
}

int gpio_read_pin(uint16_t pin)
{
        gpio_ioctl_arg_t arg;
        int rc;

        if (gpio_dev_fd < 0)
                return -1;

        arg.pin = pin;
        arg.value = 0xffff; /* sentinel to detect if the driver leaves it untouched */
        rc = ioctl(gpio_dev_fd, KD_IOCTL_READ_VALUE, &arg);
        if (rc < 0)
                return -1;
        return arg.value;
}
