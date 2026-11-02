#include "frame.h"

#include <zephyr/kernel.h>
#include <stdio.h>

#include "button_inputs.h"
#include "epoch.h"
#include "joystick.h"

#ifdef CONFIG_DEBUG_TFLITE
/* Replay mode: the two joystick values come from the
 * recorded trajectory in replay_data.c instead of the ADC. See
 * src/replay_data.h for the source data, the flash/RAM cost and - most
 * importantly - the pacing caveat that governs whether a comparison
 * against the harness's reference output is valid. */
#include "replay_data.h"
#endif

/* "timestamp" is the NTP-SYNCHRONISED wall-clock time at the moment this
 * frame was built, read from CLOCK_REALTIME via epoch_now_ms() - so it
 * advances on every frame.
 *
 * Deliberately epoch_now_ms() and NOT epoch_get_last_ntp(). The latter
 * returns the raw timestamp from the most recent NTP reply, which is only
 * written when a sync succeeds - it stands still for the whole
 * NTP_RESYNC_INTERVAL_S window (src/main.c) and then steps, so every
 * frame in between would report an identical time. CLOCK_REALTIME is the
 * same clock NTP corrects (epoch_sync_now() pushes each reply into it via
 * clock_settime()), so this value is still NTP-derived; it simply keeps
 * advancing between syncs. epoch_get_last_ntp() remains available for
 * anything that specifically needs the raw last-reply value.
 *
 * Two consequences worth knowing on the consuming side:
 *   - Between syncs the time advances on the board's local oscillator, so
 *     it drifts; each resync steps it back onto the server's time, which
 *     means timestamp can jump slightly FORWARD or BACKWARD once every
 *     NTP_RESYNC_INTERVAL_S (src/main.c). It is not guaranteed
 *     monotonic - use sequence_id for ordering, not this field.
 *     (Measured on hardware: the step at the first resync was 1 ms
 *     backwards.)
 *   - Before the first successful sync, CLOCK_REALTIME is still at its
 *     epoch-0 default, so timestamp reads a small number (seconds since
 *     boot, i.e. a 1970 date). Any year-1970 timestamp means "NTP has
 *     not synced yet".
 *
 * "transmission_time_ms" is how long the previous frame took to send -
 * published by the sender via frame_note_send_duration_ms(). A frame
 * can't report its own send duration (that time doesn't exist yet when
 * the frame is being built), so this is necessarily one frame behind:
 * frame N carries the send duration measured for frame N-1. On the very
 * first frame, before any send has happened yet, this reads 0.
 *
 * last_send_duration_ms is written by the sender thread (via
 * frame_note_send_duration_ms()) and read here on the sampler thread (via
 * frame_build()). Zephyr's atomic_t gives lock-free, torn-read-free
 * access to this single word without needing a separate lock.
 *
 * Length is VARIABLE (sequence_id, the epoch-seconds part, and
 * transmission_time_ms all grow more digits over time). See
 * MAX_FRAME_SIZE in frame.h for the worst-case byte count, field by
 * field, and the BUILD_ASSERT below that enforces it. */
static atomic_t last_send_duration_ms = ATOMIC_INIT(0);

static uint32_t frame_seq;

/* Copy of the raw counts that went into the last frame built, published
 * for logging via frame_get_last_raw(). See that function's comment in
 * frame.h for why this exists and why no lock is needed. */
static uint16_t last_x_raw;
static uint16_t last_y_raw;

#ifdef CONFIG_DEBUG_TFLITE
/* Recorded row index the last frame came from, for frame_get_last_row(). */
static uint32_t last_replay_row;
#endif

/* Self-test (compile time, zero cost at run time): the worst-case frame
 * length derived in frame.h is 297 bytes plus the NUL snprintf always
 * writes. If anyone shortens MAX_FRAME_SIZE or adds a field without
 * redoing that arithmetic, the build fails here rather than silently
 * emitting truncated - and therefore invalid - JSON at run time.
 *
 * The matching field-by-field arithmetic lives in frame.h and is
 * re-checked against a real formatted string by the frame self-test in
 * the project's test folder. */
#define FRAME_WORST_CASE_LEN 297
BUILD_ASSERT(MAX_FRAME_SIZE >= FRAME_WORST_CASE_LEN + 1,
	     "MAX_FRAME_SIZE too small for the worst-case frame - see the "
	     "field-by-field byte count in frame.h");

/* Button state carried by the frame.
 *
 * The nRF7002 DK has only TWO user buttons, so only two of the five fields
 * can be fed from it (see src/button_inputs.h for the pins):
 *
 *   DK Button 1 (sw0, P1.08)  ->  button_up
 *   DK Button 2 (sw1, P1.09)  ->  button_down
 *
 * button_trigger, button_orient and button_power have no DK button behind
 * them and report 0 ("not pressed") on every frame, so the JSON contract
 * stays fixed for the consumer. To feed one of them, assign it in
 * button_read() below; a third physical button would need an entry in
 * app.overlay, since the board file defines no more than these two.
 *
 * uint8_t rather than bool: the fields are emitted as JSON integers 0/1,
 * and uint8_t is what a gpio_pin_get_dt() result naturally narrows to. */
struct button_state {
	uint8_t trigger;
	uint8_t up;
	uint8_t down;
	uint8_t orient;
	uint8_t power;
};

static void button_read(struct button_state *b)
{
	/* Fields with no button behind them are written out one at a time
	 * rather than memset(0), so wiring each one up later is a one-line
	 * edit at an obvious spot. */
	b->trigger = 0;
	b->orient  = 0;
	b->power   = 0;

#ifdef CONFIG_DEBUG_TFLITE
	/* Replay build: all five button columns are 0 in every one of the 1151
	 * rows of controller_data_log.csv (checked, not assumed), so report 0
	 * here too - otherwise a real press during a replay would put a 1 in a
	 * frame that is supposed to reproduce the recorded input exactly. */
	b->up   = 0;
	b->down = 0;
#else
	button_inputs_read(&b->up, &b->down);
#endif
}

size_t frame_build(uint8_t *out)
{
	uint64_t now_ms;
	uint64_t ntp_seconds;
	unsigned int ntp_frac_ms;
	uint32_t tx_ms = (uint32_t)atomic_get(&last_send_duration_ms);
	float x_norm;
	float y_norm;
	uint16_t x_raw;
	uint16_t y_raw;
	struct button_state btn;
	int len;

#ifdef CONFIG_DEBUG_TFLITE
	/* REPLAY MODE. All four joystick values come from
	 * one recorded row, so they remain four views of a single sample
	 * exactly as the live path guarantees below.
	 *
	 * Two properties worth relying on when checking the output:
	 *   - Exactly one row is consumed per frame, and frame_seq advances
	 *     in the same step, so on the FIRST pass "sequence_id" equals
	 *     the zero-based CSV row index. That makes each transmitted
	 *     frame directly traceable to a line of controller_data_log.csv
	 *     and to the matching line of motor_commands_output.csv.
	 *     After a wrap (CONFIG_DEBUG_TFLITE_LOOP) they diverge - use
	 *     replay_cursor() for the row index at that point.
	 *   - x_raw/y_raw here are DERIVED from the normalised pair, not
	 *     measured. They are computed by the generator as the exact
	 *     inverse of joystick.c's normalisation about the nominal 2048
	 *     centre, so the sign relationship documented in frame.h still
	 *     holds - but they say nothing about the real hardware.
	 *
	 * The live ADC is still sampled by main.c each cycle; its result is
	 * simply not used for the frame. That is deliberate: the ADC error
	 * logging in joystick.c keeps working, so a replay build can still
	 * be used to investigate the open joystick wiring question. */
	/* Read the cursor BEFORE replay_next() advances it, so the recorded
	 * row index published for logging is the row this frame actually
	 * used, not the next one. */
	last_replay_row = replay_cursor();

	const struct replay_sample *rs = replay_next();

	x_norm = rs->x;
	y_norm = rs->y;
	x_raw  = rs->x_raw;
	y_raw  = rs->y_raw;
#else
	/* Both getters return values derived from the SAME cached sample
	 * (joystick_sample() writes current_x_state/current_y_state; both
	 * getters only read them), so the normalised and raw pairs in one
	 * frame are always two views of one measurement, never two
	 * different instants. No lock needed: the sampler thread's writes
	 * are single 16-bit words.
	 *
	 * See frame.h for why both raw and normalised forms are shipped, and
	 * for the sign caveat - raw is NOT negated, normalised IS, so the two
	 * move in opposite directions on this hardware. */
	joystick_get_normalized(&x_norm, &y_norm);
	joystick_get_raw(&x_raw, &y_raw);
#endif
	/* Publish the raw pair actually used, for the sender's log line.
	 * Set here rather than in each branch so both paths are covered by
	 * one assignment and cannot drift apart. */
	last_x_raw = x_raw;
	last_y_raw = y_raw;

	button_read(&btn);

	/* "timestamp" is the NTP-synchronised wall clock at build time. One
	 * read of CLOCK_REALTIME (cheap, no network) split into whole
	 * seconds and milliseconds - taken ONCE into now_ms so the two
	 * printed halves can never come from two different reads and
	 * disagree at a second boundary. No Q32 conversion is needed any
	 * more: epoch_now_ms() already returns milliseconds. */
	now_ms = epoch_now_ms();
	ntp_seconds = now_ms / 1000ULL;
	ntp_frac_ms = (unsigned int)(now_ms % 1000ULL);

	len = snprintf((char *)out, MAX_FRAME_SIZE,
			"{\"message_type\":\"joystick_controller\","
			"\"sequence_id\":%u,"
			"\"timestamp\":%llu.%03u,"
			"\"transmission_time_ms\":%u,"
			"\"joystick_x\":%.8f,"
			"\"joystick_y\":%.8f,"
			"\"joystick_x_raw\":%u,"
			"\"joystick_y_raw\":%u,"
			"\"button_trigger\":%u,"
			"\"button_up\":%u,"
			"\"button_down\":%u,"
			"\"button_orient\":%u,"
			"\"button_power\":%u}",
			frame_seq, (unsigned long long)ntp_seconds, ntp_frac_ms,
			tx_ms, (double)x_norm, (double)y_norm,
			/* uint16_t promotes to int in a variadic call; %u
			 * reads it back as unsigned int. The values are
			 * 0..4095 so the signedness round-trip is exact. */
			x_raw, y_raw,
			btn.trigger, btn.up, btn.down, btn.orient, btn.power);

	frame_seq++;

	/* Clamp in case snprintf's "would have written" return exceeds the
	 * buffer. The BUILD_ASSERT above proves this cannot happen for the
	 * worst-case field widths, so this path is defensive only - it
	 * exists so that a future field added without re-checking the
	 * arithmetic reports a length no longer than what is actually in
	 * the buffer, instead of overrunning the caller's copy. */
	if (len < 0) {
		return 0;
	}
	if ((size_t)len >= MAX_FRAME_SIZE) {
		return MAX_FRAME_SIZE - 1;
	}
	return (size_t)len;
}

uint32_t frame_get_seq(void)
{
	return frame_seq;
}

void frame_note_send_duration_ms(int64_t ms)
{
	atomic_set(&last_send_duration_ms, (atomic_val_t)ms);
}

void frame_get_last_raw(uint16_t *x_raw, uint16_t *y_raw)
{
	*x_raw = last_x_raw;
	*y_raw = last_y_raw;
}

#ifdef CONFIG_DEBUG_TFLITE
uint32_t frame_get_last_replay_row(void)
{
	return last_replay_row;
}
#endif
