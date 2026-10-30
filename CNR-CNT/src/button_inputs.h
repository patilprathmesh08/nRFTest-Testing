#ifndef BUTTON_INPUTS_H_
#define BUTTON_INPUTS_H_

#include <stdint.h>

/* ---------------- Button states reported in the telemetry frame ----------
 *
 * Reads the nRF7002 DK's two user buttons so frame.c can report them in the
 * "button_*" JSON fields. This is separate from src/button.c, which uses
 * Button 1 as a send TRIGGER (when a frame goes out) and is compiled only
 * in the button send-trigger modes; this module is always compiled and
 * reports the button STATE inside every frame.
 *
 * The DK has only TWO user buttons (board devicetree, nrf5340_cpuapp_common.dtsi):
 *
 *   silkscreen "Button 1" -> node button0 -> alias sw0 -> P1.08
 *   silkscreen "Button 2" -> node button1 -> alias sw1 -> P1.09
 *
 * (Note the numbering trap: the node NAMED button1 is silkscreen "Button 2".)
 * The frame has five button fields, so three of them have no DK button to
 * read - see button_read() in src/frame.c for which fields are fed and
 * which stay 0.
 *
 * Both pins are GPIO_ACTIVE_LOW | GPIO_PULL_UP in the board file, so a
 * press shorts to ground. gpio_pin_get_dt() applies that inversion and
 * returns the LOGICAL level, so 1 = pressed. Not inverted again here.
 *
 * Levels are sampled raw, with no debounce: each frame reports "is the
 * button down at this instant", and a bounce can only mis-report one
 * frame (it self-corrects on the next). Debounce matters for counting
 * edges, which is src/button.c's job, not this module's.
 */

/* Configures both button GPIOs as inputs. Call once at boot, before the
 * sampler starts. Returns 0 on success, or a negative errno if either pin
 * could not be configured. A failure is not fatal to telemetry - the
 * affected button simply reports 0 (not pressed). */
int button_inputs_init(void);

/* Current state of DK Button 1 and Button 2: 1 = pressed, 0 = not pressed
 * (also 0 for any button whose init failed). Either pointer may be passed
 * as NULL if only the other is wanted. */
void button_inputs_read(uint8_t *button1, uint8_t *button2);

#endif /* BUTTON_INPUTS_H_ */
