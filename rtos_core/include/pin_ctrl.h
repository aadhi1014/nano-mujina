#ifndef PIN_CTRL_H
#define PIN_CTRL_H

#include <stdint.h>

void gpio_init(uint8_t cnt);
void gpio_set_value(uint8_t m, uint8_t value);

#define ENTER_CONFIG_MODE(i) do { gpio_set_value(i, 1); } while (0)
#define ENTER_WORK_MODE(i)   do { gpio_set_value(i, 0); } while (0)

/*
 * ASIC chain RST -- GPIO31, Bank A, output, ACTIVE-HIGH (held LOW during
 * normal operation, chips running; HIGH = held in reset).
 */
#define ASIC_RST_PIN 31

int gpio_asic_rst_init(void);
void gpio_asic_rst_assert(void);   /* HIGH -- hold chips in reset */
void gpio_asic_rst_deassert(void); /* LOW -- release, chips run */
int gpio_read_pin(uint16_t pin);   /* -1 on failure, else 0/1 */

/*
 * power_en (GPIO34) is controlled via /sys/class/gpio by the harness/
 * Linux side, not by this file.
 */

#endif
