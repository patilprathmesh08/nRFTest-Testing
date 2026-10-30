#include "led.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(led, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/drivers/gpio.h>
#include <errno.h>

/* Uses the "led0" alias already defined in the nRF7002 DK's board
 * devicetree (LED1 on the silkscreen, GPIO P1.06 per Table 6 of the
 * Hardware User Guide) - no overlay changes needed, this alias ships
 * with the board support. */
static const struct gpio_dt_spec send_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int led_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&send_led)) {
		LOG_ERR("Send-activity LED device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&send_led, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Failed to configure send-activity LED (%d)", ret);
		return ret;
	}

	return 0;
}

void led_on(void)
{
	gpio_pin_set_dt(&send_led, 1);
}

void led_off(void)
{
	gpio_pin_set_dt(&send_led, 0);
}
