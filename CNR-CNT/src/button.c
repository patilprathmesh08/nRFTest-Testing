#include "button.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(button, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/drivers/gpio.h>
#include <errno.h>

/* Uses the "sw0" alias from the nRF7002 DK's board devicetree, which is
 * node `button0`, silkscreen "Button 1", P1.08. See the header for why
 * this is sw0 and NOT sw1 despite a node named `button1` existing - that
 * one is the second button on the board.
 *
 * Same pattern as src/led.c's DT_ALIAS(led0): the alias ships with the
 * board support, so no app.overlay entry is needed. */
static const struct gpio_dt_spec send_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Consecutive identical polls required before a level change is accepted.
 *
 * 2 ticks = 40 ms at the current 20 ms sampler period, comfortably longer
 * than the ~5 ms a tactile switch takes to settle, and still far below
 * the ~150 ms floor of a deliberate human press - so no real press is
 * ever swallowed. Expressed in TICKS rather than milliseconds so it
 * tracks the sampler period automatically instead of quietly becoming
 * meaningless if that period changes. */
#define BUTTON_DEBOUNCE_TICKS 2

/* Guards the accessors against being used before a successful
 * button_init(). Without it the first tick would read an unconfigured
 * pin, whose value is whatever the pad happened to power up as. */
static bool button_ready;

/* Debounce state machine, all touched only from the sampler thread via
 * button_poll(), so none of it needs locking.
 *   last_raw     - level seen on the previous poll, to spot instability
 *   stable_count - how many consecutive polls have agreed with last_raw
 *   stable_state - the accepted, debounced level (true = pressed)
 *   press_latch  - set on an accepted rising edge, cleared when consumed
 */
static bool    last_raw;
static uint8_t stable_count;
static bool    stable_state;
static bool    press_latch;

int button_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&send_button)) {
		LOG_ERR("Send-trigger button device not ready (sw0 / P1.08)");
		return -ENODEV;
	}

	/* GPIO_INPUT only. The pull-up and active-low polarity both come from
	 * the board devicetree flags (0x11) and gpio_pin_configure_dt() ORs
	 * them in - restating them here would be redundant, and hardcoding
	 * polarity the DT already declares is how a board port with different
	 * wiring silently breaks. */
	ret = gpio_pin_configure_dt(&send_button, GPIO_INPUT);
	if (ret) {
		LOG_ERR("Failed to configure send-trigger button (%d)", ret);
		return ret;
	}

	button_ready = true;

	/* Stated at boot because the behaviour change is otherwise invisible:
	 * someone who flashes this and sees no traffic needs to be told the
	 * board is waiting on a button, not broken. */
#ifdef CONFIG_SEND_TRIGGER_BUTTON_PRESS
	LOG_INF("Send trigger armed on Button 1 (sw0 / P1.08) - ONE frame per "
		"press, %d-tick debounce", BUTTON_DEBOUNCE_TICKS);
#else
	LOG_INF("Send gate armed on Button 1 (sw0 / P1.08) - frames sent ONLY "
		"while held, %d-tick debounce", BUTTON_DEBOUNCE_TICKS);
#endif

	return 0;
}

void button_poll(void)
{
	int val;
	bool raw;

	if (!button_ready) {
		return; /* accessors already fail safe; nothing to update */
	}

	/* LOGICAL level: 1 means pressed even though the pin is electrically
	 * low when the button shorts to ground. The active-low inversion is
	 * applied inside this call from the DT flags. */
	val = gpio_pin_get_dt(&send_button);
	if (val < 0) {
		/* Not rate-limited on purpose: a failing GPIO read cannot
		 * happen on a working board, and at 50 Hz the resulting log
		 * flood IS the alarm. Leave the debounce state untouched so a
		 * one-off glitch cannot fabricate an edge. */
		LOG_ERR("Send-trigger button read failed (%d)", val);
		return;
	}
	raw = (val == 1);

	if (raw != last_raw) {
		/* Level moved - restart the stability count. This is the
		 * branch contact bounce lands in, and it is why a bouncing
		 * press never reaches the accept path below. */
		last_raw = raw;
		stable_count = 0;
		return;
	}

	if (stable_count < BUTTON_DEBOUNCE_TICKS) {
		stable_count++;

		/* Accept exactly once, on the poll where the count REACHES the
		 * threshold. Testing for equality rather than >= is what stops
		 * a held button from re-latching a press on every later tick. */
		if (stable_count == BUTTON_DEBOUNCE_TICKS && raw != stable_state) {
			stable_state = raw;

			if (raw) {
				/* Rising edge only. A release must be seen and
				 * debounced before another press can latch. */
				press_latch = true;
			}
		}
	}
}

bool button_is_held(void)
{
	return button_ready && stable_state;
}

bool button_take_press(void)
{
	bool pressed = press_latch;

	/* Consume unconditionally: clearing even when it was already false
	 * costs nothing and makes a double-send structurally impossible. */
	press_latch = false;

	return pressed;
}
