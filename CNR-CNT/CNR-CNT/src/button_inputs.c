#include "button_inputs.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(button_inputs, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/drivers/gpio.h>
#include <stdbool.h>
#include <errno.h>

/* Both aliases ship with the nRF7002 DK board support, so no app.overlay
 * entry is needed (same pattern as src/led.c and src/button.c). */
static const struct gpio_dt_spec button1_spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button2_spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

/* Set only after the pin was configured successfully. Without it a read
 * before init (or after a failed init) would sample an unconfigured pad,
 * whose value is whatever it powered up as; this makes it report 0. */
static bool button1_ready;
static bool button2_ready;

/* Configures one pin as an input. GPIO_INPUT only: the pull-up and the
 * active-low polarity both come from the board devicetree flags and
 * gpio_pin_configure_dt() ORs them in, so restating them here would be
 * redundant. Returns 0 on success. */
static int configure_input(const struct gpio_dt_spec *spec, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("%s GPIO device not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (ret) {
		LOG_ERR("Failed to configure %s (%d)", name, ret);
	}
	return ret;
}

int button_inputs_init(void)
{
	int ret1 = configure_input(&button1_spec, "Button 1 (sw0 / P1.08)");
	int ret2 = configure_input(&button2_spec, "Button 2 (sw1 / P1.09)");

	button1_ready = (ret1 == 0);
	button2_ready = (ret2 == 0);

	if (button1_ready && button2_ready) {
		LOG_INF("Button state inputs ready: Button 1 (P1.08), Button 2 (P1.09)");
	}

	return ret1 ? ret1 : ret2;
}

/* Logical level of one pin: 1 = pressed. A negative gpio_pin_get_dt()
 * result is a read error and is reported as "not pressed" rather than
 * propagated - a button state is not worth failing a frame over. */
static uint8_t read_pressed(const struct gpio_dt_spec *spec, bool ready)
{
	int val;

	if (!ready) {
		return 0;
	}

	val = gpio_pin_get_dt(spec);
	return (val > 0) ? 1 : 0;
}

void button_inputs_read(uint8_t *button1, uint8_t *button2)
{
	if (button1) {
		*button1 = read_pressed(&button1_spec, button1_ready);
	}
	if (button2) {
		*button2 = read_pressed(&button2_spec, button2_ready);
	}
}
