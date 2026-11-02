#ifndef FRAME_H_
#define FRAME_H_

#include <stddef.h>
#include <stdint.h>

/* JSON telemetry frame builder.
 *
 * JSON text frame matching this exact shape:
 *
 *   {"message_type":"joystick_controller","sequence_id":<uint32>,
 *    "timestamp":<NTP-synced epoch seconds.millis at build time,
 *                 e.g. 1787815738.632 - advances every frame>,
 *    "transmission_time_ms":<uint32>,
 *    "joystick_x":<-1.0 to 1.0>,"joystick_y":<-1.0 to 1.0>,
 *    "joystick_x_raw":<uint16, 0..4095>,
 *    "joystick_y_raw":<uint16, 0..4095>,
 *    "button_trigger":<0|1>,"button_up":<0|1>,"button_down":<0|1>,
 *    "button_orient":<0|1>,"button_power":<0|1>}
 *
 * "joystick_x_raw" / "joystick_y_raw" are the unscaled 12-bit ADC counts
 * (0..4095) straight from joystick_get_raw() (src/joystick.h) - NOT
 * sign-corrected and NOT centre-corrected, i.e. what the ADC actually
 * measured.
 *
 *     Why both raw and normalised: the normalised floats hide what the
 *     hardware is doing.
 *     A raw value near 0 means the input is near ground; near 4095 means
 *     near full scale; ~2048 is a healthy resting divider. Shipping the
 *     raw counts lets the consumer (and anyone reading a capture) tell a
 *     genuinely centred joystick apart from a disconnected one, which
 *     the normalised value cannot express - both look like ~0.0 once the
 *     boot calibration has taken the bad resting value as its zero.
 *     This is directly relevant here: on 03-09-2026 the board calibrated
 *     x_center=152, y_center=162 out of 4095, i.e. the joystick input was
 *     sitting near ground rather than at mid-supply, and the normalised
 *     output gave no hint of it. Confirmed again on 07-09-2026, when the
 *     live ADC was measured between 46 and 85 over 43 consecutive frames -
 *     a wiring or reference fault on the joystick, not a firmware one.
 *
 *     Note the raw pair is NOT negated, while the normalised pair is
 *     (see joystick_get_normalized() in src/joystick.c) - on this
 *     hardware the ADC voltage RISES when the stick is pushed left/down.
 *     So raw and normalised move in OPPOSITE directions. Expected, not
 *     a bug.
 *
 * Button fields: "button_up" is DK Button 1 and "button_down" is DK
 * Button 2 (1 = pressed, 0 = not pressed). "button_trigger",
 * "button_orient" and "button_power" have no DK button behind them (the
 * DK has only two) and always emit 0. In a CONFIG_DEBUG_TFLITE replay
 * build all five emit 0. See button_read() in frame.c and
 * src/button_inputs.h.
 *
 * See frame.c for the field-by-field rationale (why "timestamp" is read
 * from the NTP-corrected CLOCK_REALTIME rather than the raw last-reply
 * value, why it is not guaranteed monotonic across a resync, why
 * "transmission_time_ms" is necessarily one frame behind, etc.).
 */

/* Max size in bytes of one built frame - shared with callers that size
 * their own buffers around it (the sampler/sender shared buffer in
 * main.c, the WebSocket framing buffer in src/ws.c).
 *
 * Worst-case length, counted field by field (all counts include the
 * separating comma):
 *
 *   {"message_type":"joystick_controller",        38
 *   "sequence_id":<10 digits>,                    25
 *   "timestamp":<11 digits>.<3 digits>,           28
 *   "transmission_time_ms":<10 digits>,           34
 *   "joystick_x":-1.0000,                         21
 *   "joystick_y":-1.0000,                         21
 *   "joystick_x_raw":<5 digits>,                  23
 *   "joystick_y_raw":<5 digits>,                  23
 *   "button_trigger":0,                           19
 *   "button_up":0,                                14
 *   "button_down":0,                              16
 *   "button_orient":0,                            18
 *   "button_power":0}                             17
 *                                                ---
 *                                                297  + 1 NUL = 298
 *
 * The raw fields are budgeted at 5 digits, not 4. The ADC is 12-bit so
 * the real range is 0..4095 (4 digits), but the values are uint16_t and
 * are printed with %u, so a bad driver read could in principle print up
 * to 65535. Budgeting the TYPE's worst case rather than the hardware's
 * keeps the BUILD_ASSERT in frame.c a genuine proof of no truncation.
 *
 * 320 leaves 22 bytes spare. A typical frame is ~274 bytes (measured by
 * the frame self-test in the project's test folder, not estimated); the
 * extra headroom covers digit growth in sequence_id and
 * transmission_time_ms.
 *
 * COST: this buffer is instantiated once as static RAM (shared_frame in
 * main.c) and three times on thread stacks (main.c sender local_frame,
 * main.c sampler frame, ws.c buf[8 + MAX_FRAME_SIZE]). Every byte added
 * here therefore costs one byte of static RAM plus one on each of those
 * stacks - which matters, because RAM sits at roughly 98.4 %. Sender
 * stack is 6144 B and main stack 6144 B, so stack growth is comfortably
 * absorbed; the static cost is the part that eats real
 * headroom. */
#define MAX_FRAME_SIZE 320

/* Fills out[] with the JSON frame text and returns the actual length
 * written (<= MAX_FRAME_SIZE). Reads the joystick's current normalized
 * position AND its raw ADC counts (src/joystick.h) from the same cached
 * sample, the most recent NTP timestamp (src/epoch.h),
 * the current DK button states (see button_read() in frame.c), and the previous
 * frame's send duration (see frame_note_send_duration_ms() below). Also
 * advances the internal sequence counter - see frame_get_seq(). */
size_t frame_build(uint8_t *out);

/* The sequence_id that will be assigned to the NEXT frame_build() call
 * (i.e. frame_build() increments this after using it) - callers wanting
 * "the frame that was just built/sent" use frame_get_seq() - 1. */
uint32_t frame_get_seq(void);

/* Publishes how long the most recently sent frame took to send, in
 * milliseconds, so the NEXT frame_build() call can include it as
 * "transmission_time_ms". A frame can't report its own send duration
 * (that time doesn't exist yet while it's being built), so this is
 * necessarily one frame behind. */
void frame_note_send_duration_ms(int64_t ms);

/* The raw counts that went into the MOST RECENTLY BUILT frame, for
 * logging.
 *
 * Use this, NOT joystick_get_raw(), anywhere the intent is "what did we
 * just send". joystick_get_raw() reads the LIVE ADC, which is a different
 * thing: in a normal build the two agree closely enough to be
 * indistinguishable, but in a CONFIG_DEBUG_TFLITE replay build they are
 * unrelated numbers - the live ADC versus the recorded row that actually
 * went out - and a log built on the wrong one will claim the replay is
 * broken when the payload is perfectly correct.
 *
 * Written on the sampler thread and read on the sender thread without a
 * lock, exactly as the joystick module's cached state already is. A torn
 * read would at worst pair an x from one frame with a y from the next,
 * which is acceptable for a log line and nothing else. Do not use these
 * values for control. */
void frame_get_last_raw(uint16_t *x_raw, uint16_t *y_raw);

#ifdef CONFIG_DEBUG_TFLITE
/* Zero-based index of the recorded row that the most recently built frame
 * came from, so a log line can be matched against controller_data_log.csv
 * directly. Only meaningful in a replay build. */
uint32_t frame_get_last_replay_row(void);
#endif

#endif /* FRAME_H_ */
