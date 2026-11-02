#include "joystick.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(joystick, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include <errno.h>

/* X is read on AIN0 (P0.04, board Arduino label A0); Y is read on AIN1
 * (P0.05, board Arduino label A1) - per Table 5 of the nRF7002 DK Hardware
 * User Guide. */
#define ADC_NODE DT_PATH(zephyr_user)

#if !DT_NODE_EXISTS(ADC_NODE)
#error "zephyr,user node with io-channels not found - add the ADC overlay (see notes)"
#endif

static const struct adc_dt_spec adc_channel_x = ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 0);
static const struct adc_dt_spec adc_channel_y = ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 1);

/* Current X/Y state, updated once per joystick_sample() call.
 * Not volatile / not touched from ISR context — this build polls the ADC
 * synchronously (blocking read). uint16_t: holds the full 12-bit ADC
 * range (0-4095) unscaled. */
static uint16_t current_x_state;
static uint16_t current_y_state;

/* The JSON "joystick_x"/"joystick_y" fields send a normalized float
 * instead of the raw 12-bit ADC value, so anything consuming the data
 * (game logic, a control loop, etc.) doesn't need to know anything about
 * ADC resolution, gain, or voltage - just: 0.0 = centered, -1.0/+1.0 =
 * fully deflected one way or the other.
 *
 * The center point is not a hardcoded theoretical constant (2048): the
 * joystick's actual resting raw value is slightly off that theoretical
 * midpoint on both axes (small potentiometer/ADC offset error - a few
 * percent, not a bug), which would otherwise make "resting" report small
 * nonzero values like -0.018 instead of a clean 0.0. joystick_calibrate_center()
 * below takes a burst of real readings on both axes right after ADC init
 * (before the joystick is expected to have been touched yet) and averages
 * them into x_center/y_center, which the normalization below then uses as
 * the true zero-point per axis.
 *
 * ADC_HALF_RANGE stays a fixed theoretical value (still correct given
 * the ADC_GAIN_1_3 overlay setting matching the joystick's real 1.8V
 * swing) - only the CENTER needed real-world calibration, not the
 * overall span. */
#define ADC_HALF_RANGE           2048
#define ADC_CALIBRATION_SAMPLES  32
#define ADC_CALIBRATION_DELAY_MS 2

static uint16_t x_center = 2048;  /* overwritten by joystick_calibrate_center() */
static uint16_t y_center = 2048;  /* overwritten by joystick_calibrate_center() */

int joystick_init(void)
{
	int ret;

	if (!adc_is_ready_dt(&adc_channel_x)) {
		LOG_ERR("ADC X channel (AIN0/P0.04) device not ready");
		return -ENODEV;
	}
	if (!adc_is_ready_dt(&adc_channel_y)) {
		LOG_ERR("ADC Y channel (AIN1/P0.05) device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&adc_channel_x);
	if (ret) {
		LOG_ERR("Failed to setup ADC X channel (%d)", ret);
		return ret;
	}

	ret = adc_channel_setup_dt(&adc_channel_y);
	if (ret) {
		LOG_ERR("Failed to setup ADC Y channel (%d)", ret);
		return ret;
	}

	return 0;
}

/* Reads one raw sample from a single ADC channel. Shared by both
 * joystick_sample() (the per-cycle read) and joystick_calibrate_center(),
 * so there's exactly one place that does the actual
 * adc_sequence_init_dt()/adc_read_dt() call. Returns 0 on success and
 * writes the raw 12-bit value into *out; on failure, *out is left
 * untouched and the error is returned (not logged here - each caller
 * decides how to handle/log a failure in its own context). */
static int adc_read_raw(const struct adc_dt_spec *chan, uint16_t *out)
{
	int err;
	uint16_t buf;
	struct adc_sequence sequence = {
		.buffer      = &buf,
		.buffer_size = sizeof(buf),
	};

	err = adc_sequence_init_dt(chan, &sequence);
	if (err) {
		return err;
	}
	err = adc_read_dt(chan, &sequence);
	if (err) {
		return err;
	}

	*out = buf;
	return 0;
}

void joystick_sample(void)
{
	int err;
	uint16_t buf_x;
	uint16_t buf_y;

	err = adc_read_raw(&adc_channel_x, &buf_x);
	if (err) {
		LOG_ERR("X-ADC read failed (%d)", err);
		return;
	}

	err = adc_read_raw(&adc_channel_y, &buf_y);
	if (err) {
		LOG_ERR("Y ADC read failed (%d)", err);
		return;
	}

	/* buf_x/buf_y are raw 12-bit readings (0-4095) - stored as-is, no
	 * scaling. */
	current_x_state = buf_x;
	current_y_state = buf_y;
}

void joystick_get_raw(uint16_t *x, uint16_t *y)
{
	*x = current_x_state;
	*y = current_y_state;
}

void joystick_calibrate_center(void)
{
	uint32_t x_sum = 0;
	uint32_t y_sum = 0;
	int x_ok = 0;
	int y_ok = 0;
	uint16_t sample;
	int i;

	LOG_INF("Calibrating joystick center (%d samples, joystick should be "
		"at rest)...", ADC_CALIBRATION_SAMPLES);

	for (i = 0; i < ADC_CALIBRATION_SAMPLES; i++) {
		if (adc_read_raw(&adc_channel_x, &sample) == 0) {
			x_sum += sample;
			x_ok++;
		}
		if (adc_read_raw(&adc_channel_y, &sample) == 0) {
			y_sum += sample;
			y_ok++;
		}
		k_sleep(K_MSEC(ADC_CALIBRATION_DELAY_MS));
	}

	if (x_ok > 0) {
		x_center = (uint16_t)(x_sum / x_ok);
	} else {
		LOG_WRN("X calibration got no successful samples - keeping "
			"theoretical default (2048)");
	}
	if (y_ok > 0) {
		y_center = (uint16_t)(y_sum / y_ok);
	} else {
		LOG_WRN("Y calibration got no successful samples - keeping "
			"theoretical default (2048)");
	}

	LOG_INF("Calibration done: x_center=%u (%d/%d ok), y_center=%u "
		"(%d/%d ok)", x_center, x_ok, ADC_CALIBRATION_SAMPLES,
		y_center, y_ok, ADC_CALIBRATION_SAMPLES);
}

static float adc_to_normalized(uint16_t raw, uint16_t center)
{
	float norm = ((float)raw - center) / ADC_HALF_RANGE;

	/* Clamp in case the real extremes overshoot slightly past 0/4095
	 * equivalents (potentiometer tolerance, ADC offset error, etc.) -
	 * consumers of this value should never see anything outside
	 * [-1.0, 1.0]. */
	if (norm > 1.0f) {
		norm = 1.0f;
	}
	if (norm < -1.0f) {
		norm = -1.0f;
	}
	return norm;
}

void joystick_get_normalized(float *x, float *y)
{
	/* Both axes are negated here, after adc_to_normalized(): the raw ADC
	 * voltage increases when the joystick is pushed LEFT or DOWN (an
	 * artifact of how these specific potentiometers are wired, not a
	 * math error), so without the flip +1.0 would mean "left"/"down"
	 * instead of the normal convention where positive X = right and
	 * positive Y = up. Negating only the final reported value keeps the
	 * ADC reading, calibration, and x_center/y_center untouched. */
	*x = -adc_to_normalized(current_x_state, x_center);
	*y = -adc_to_normalized(current_y_state, y_center);
}
