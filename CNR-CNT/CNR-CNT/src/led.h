#ifndef LED_H_
#define LED_H_

/* Send-activity LED: blinks the board's LED0 for the duration of each
 * frame send, so there's a visible heartbeat that data is actually going
 * out - useful at a glance without needing to watch the serial log. */

/* Sets up the LED GPIO. Call once at boot. Returns 0 on success. Failure
 * here is meant to be treated as non-fatal by the caller - a missing LED
 * shouldn't halt telemetry, it's a nice-to-have indicator. */
int led_init(void);

/* Turns the send-activity LED on / off. Safe to call even if led_init()
 * was never called or failed - the underlying GPIO call is simply a
 * no-op/error in that case. */
void led_on(void);
void led_off(void);

#endif /* LED_H_ */
