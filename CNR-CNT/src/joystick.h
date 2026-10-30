#ifndef JOYSTICK_H_
#define JOYSTICK_H_

#include <stdint.h>

/* Joystick X/Y input module: reads the two ADC channels the joystick's
 * potentiometers are wired to, calibrates their resting center at boot,
 * and converts raw readings to a normalized, sign-corrected -1.0..1.0
 * range.
 *
 * REQUIRES a devicetree overlay defining a "zephyr,user" node with
 * io-channels pointing at &adc channel 0 (AIN0, X) and channel 1 (AIN1,
 * Y), and CONFIG_ADC=y in prj.conf.
 */

/* Sets up both ADC channels. Call once at boot, before
 * joystick_calibrate_center() or joystick_sample(). Returns 0 on success. */
int joystick_init(void);

/* Takes a burst of real readings on both axes and averages them into the
 * true resting center per axis, used by joystick_get_normalized() as the
 * zero-point. Call once at boot, after joystick_init() and BEFORE the
 * joystick is expected to be touched (it's assumed to be at rest). */
void joystick_calibrate_center(void);

/* Reads both ADC channels (blocking, one-shot each) and updates the
 * cached current state that joystick_get_raw()/joystick_get_normalized()
 * return. On a read failure, the previous state is simply left in place
 * for this cycle (logged internally, not fatal). */
void joystick_sample(void);

/* Raw 12-bit ADC readings (0-4095) from the most recent joystick_sample(),
 * unscaled and not sign-corrected. */
void joystick_get_raw(uint16_t *x, uint16_t *y);

/* The most recent joystick_sample() reading, converted to a normalized,
 * sign-corrected -1.0..1.0 range: 0.0 = centered (per the calibrated
 * center), +1.0 = fully right / up, -1.0 = fully left / down. */
void joystick_get_normalized(float *x, float *y);

#endif /* JOYSTICK_H_ */
