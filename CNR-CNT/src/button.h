#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdbool.h>

/* ---------------- Send-trigger button ----------------
 *
 * Drives frame transmission from the nRF7002 DK's silkscreen "Button 1".
 * Compiled in ONLY when one of the button send-trigger modes is selected
 * (see the SEND_TRIGGER choice in Kconfig and target_sources_ifdef in
 * CMakeLists.txt); a continuous-stream build carries none of this code.
 *
 * Two modes are served by this one module:
 *   CONFIG_SEND_TRIGGER_BUTTON_PRESS - one frame per press
 *                                      -> use button_take_press()
 *   CONFIG_SEND_TRIGGER_BUTTON_HOLD  - stream while held
 *                                      -> use button_is_held()
 *
 * WHICH BUTTON, and why it is not the one you would guess:
 *
 *   silkscreen "Button 1" -> DT node `button0` -> alias `sw0` -> P1.08
 *   silkscreen "Button 2" -> DT node `button1` -> alias `sw1` -> P1.09
 *
 * The devicetree node NAMED `button1` is the SECOND button on the board.
 * This module deliberately uses DT_ALIAS(sw0) - node `button0` - because
 * that is the one labelled "Push button 1" in
 * zephyr/boards/nordic/nrf7002dk/nrf5340_cpuapp_common.dtsi:49. Getting
 * this backwards is the easiest mistake to make here.
 *
 * The pin is GPIO_ACTIVE_LOW | GPIO_PULL_UP (flags 0x11) in that same
 * board file, so it shorts to ground when pressed. gpio_pin_get_dt()
 * applies that inversion itself and returns the LOGICAL level, so
 * 1 = pressed. Do not invert it again here.
 *
 * WHY POLLED, NOT INTERRUPT-DRIVEN:
 *
 * The sampler already ticks at a fixed period, so a poll costs one GPIO
 * read on a timer the application runs anyway. An ISR pair buys nothing
 * in return: there is no power argument (CONFIG_PM=n, the CPU is not
 * sleeping between ticks), and edge interrupts make contact bounce HARDER
 * to handle, not easier - a bounced pair of edges can leave an ISR-tracked
 * state machine inverted, whereas a resampled level always self-corrects.
 *
 * DEBOUNCE - why it is explicit rather than left to the poll rate:
 *
 * BUTTON_HOLD only needs a level ("is it down right now"), where a stray
 * sample costs at most one tick and fixes itself. BUTTON_PRESS counts
 * EDGES, and an edge miscounted is a whole extra frame transmitted.
 * Typical tactile
 * bounce settles within about 5 ms, but a poll can land inside that
 * window, so a raw press could read as press-release-press and send two
 * frames. BUTTON_DEBOUNCE_TICKS requires the level to be stable for that
 * many consecutive polls before a change is accepted.
 */

/* Configures the button GPIO. Call once at boot, before the sampler timer
 * starts. Returns 0 on success, or a negative errno.
 *
 * Treat failure as FATAL in a button-triggered build: if the pin cannot be
 * read, nothing can ever open the send path and the board would sit there
 * associated and permanently silent, looking exactly like a network
 * fault. Better to halt and say so. */
int button_init(void);

/* Samples the pin and updates the debounce state machine. Call EXACTLY
 * ONCE per sampler tick, before button_is_held() or button_take_press().
 *
 * Debounce is measured in ticks, not milliseconds, so it scales with
 * whatever the sampler period is - it does not silently become useless if
 * the period changes. */
void button_poll(void);

/* True while the button is held down, debounced. For
 * CONFIG_SEND_TRIGGER_BUTTON_HOLD. Reads state only - call button_poll()
 * first. Fails safe to false if button_init() never succeeded, which
 * gates transmission OFF; defaulting to "held" would silently resume a
 * continuous stream, which is the failure nobody would notice. */
bool button_is_held(void);

/* Returns true EXACTLY ONCE per debounced press, then clears the pending
 * flag. For CONFIG_SEND_TRIGGER_BUTTON_PRESS.
 *
 * Consume-on-read is deliberate: it makes double-sending structurally
 * impossible even if a caller invokes this twice in one tick. Only the
 * rising edge latches - holding the button down does NOT repeat, so a
 * long press still sends exactly one frame. Releasing and pressing again
 * is required for the next. */
bool button_take_press(void);

#endif /* BUTTON_H_ */
