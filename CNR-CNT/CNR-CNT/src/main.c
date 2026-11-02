#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sta, CONFIG_LOG_DEFAULT_LEVEL);

#include <nrfx_clock.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "button_inputs.h"
#include "epoch.h"
#include "frame.h"
#include "https_conn.h"
#include "joystick.h"
#include "led.h"
#ifdef CONFIG_SEND_TRIGGER_NEEDS_BUTTON
#include "button.h"
#endif
#include "wifi_net.h"

/* Drives both the sampler timer and the send cadence - the two-thread
 * architecture, ADC reads, and frame building all key off it, so nothing
 * else needs adjusting to change the rate. */
#ifdef CONFIG_DEBUG_TFLITE
/* Replay builds take the period from Kconfig so the recorded 40 Hz rate
 * can be reproduced (CONFIG_DEBUG_TFLITE_PERIOD_MS=25) without editing
 * source. Defaults to the same 1000 ms as a normal build, which means one
 * full 1151-row pass takes about 19 minutes - see the pacing caveat in
 * src/replay_data.h for when that matters and when it does not. */
#define SEND_PERIOD_MS      CONFIG_DEBUG_TFLITE_PERIOD_MS
#else
#define SEND_PERIOD_MS      100
#endif

/* NTP_SERVER points at the local ntp_server.py running on the same PC as
 * the WebSocket server, so the board doesn't depend on internet
 * reachability for time sync. Independent of the WSS server address
 * (WSS_HOST/WSS_PORT in src/https_conn.c) on purpose - they happen to be
 * the same PC in this deployment, but that's not a requirement. */
// #define NTP_SERVER   "192.168.40.132"
// #define NTP_SERVER   "192.168.40.95"   /* stale - the Verdin moved off .95 */
#define NTP_SERVER   "192.168.40.100"
/* The same mDNS name the WSS connection uses (WSS_HOST in
 * src/https_conn.c), so both follow the i.MX8MP across DHCP changes. A
 * hardcoded IP here goes stale silently: NTP just fails and CLOCK_REALTIME
 * sits at epoch 0 (1970), which shows up as a 1970 date in every frame
 * rather than as an obvious error.
 *
 * This needs no resolution code here: sntp_simple() - which is all
 * epoch_sync_now() calls, see src/epoch.c - passes its server argument
 * straight to net_getaddrinfo_addr_str()
 * (zephyr/subsys/net/lib/sntp/sntp_simple.c:86), so it resolves names
 * itself. The mDNS Kconfig that makes ".local" resolvable is documented in
 * prj.conf under "mDNS (.local) name resolution".
 *
 * DIFFERENCE FROM THE WSS PATH, deliberate: a failed NTP sync is NOT
 * retried every 5 s like https_conn_open() retries the connection. NTP
 * failure is already non-fatal here (main() logs a warning and carries on),
 * and the resync thread started below re-queries every
 * NTP_RESYNC_INTERVAL_S anyway, which is the existing recovery path. */

/* How often the epoch module re-queries NTP after the initial boot-time
 * sync, to correct for local oscillator drift over long (hours/days) runs.
 *
 * NTP_SERVER is a local/private server on the same network, not a public
 * one, so there's no politeness floor to respect (RFC guidance against
 * hammering public NTP servers doesn't apply here).
 *
 * The resync runs on its own thread (started via epoch_start_resync() in
 * main(), see src/epoch.c) at NTP_RESYNC_PRIORITY below - not on the
 * sampler or sender thread - so it never affects the sampling cadence. */
/* This is a private NTP server on the same LAN, so the politeness floor
 * that applies to public pools does not apply - the interval is chosen
 * purely to bound how long CLOCK_REALTIME can stay wrong after a
 * transient failure.
 *
 * FLOOR: do not shorten this below about 6 s. A FAILING sync blocks the
 * resync thread for up to NTP_SYNC_TIMEOUT_MS (5000, src/epoch.c), so a
 * shorter interval would have one attempt still in progress when the next
 * is due. Even at that floor nothing else suffers - the thread runs at
 * the lowest of the three application priorities and cannot delay the
 * sampler or the sender.
 *
 * The frame's "timestamp" steps by the accumulated oscillator drift once
 * per interval, so a longer interval means fewer but larger steps. It is
 * never monotonic across a resync either way - see src/frame.c. */
#define NTP_RESYNC_INTERVAL_S 100
#define NTP_RESYNC_PRIORITY   8  /* lower priority than sender (7) and the
				  * sampler on the main thread, so this thread
				  * never preempts either */

/* ---------------- Two-thread architecture for jitter-free 10ms --------
 *
 * Problem: with one thread doing both sampling and network I/O, a single
 * slow TCP round trip (e.g. 25ms Wi-Fi retransmission) delays the NEXT
 * sample too, because frame_build() (src/frame.c) can't run while
 * https_conn_send_frame() (src/https_conn.c) is blocked. The timestamp
 * inside the frame reflects when it was actually built, not when it
 * should have been built - so the server sees jitter.
 *
 * Solution: two threads, one shared buffer, plus the epoch module's own
 * periodic NTP resync thread (src/epoch.c, started via epoch_start_resync()
 * in main() below). CLOCK_REALTIME is independent of
 * sample_timer/sample_tick/frame_ready, so NTP resync never touches this
 * section at all.
 *
 * SAMPLER (main thread, timer-driven, never touches the network):
 *   - k_timer fires every 10ms, gives a semaphore
 *   - main loop takes the semaphore, reads joystick, builds frame
 *   - stores the frame in a shared buffer (protected by a mutex)
 *   - this thread NEVER blocks on network I/O, so the timer cadence
 *     is perfectly maintained regardless of what the network is doing
 *
 * SENDER (dedicated thread, runs as fast as it can):
 *   - waits on a "new frame available" semaphore
 *   - copies the latest frame from the shared buffer
 *   - sends it over HTTPS, waits for response (blocking is fine here -
 *     this thread has nothing else to do, and blocking here doesn't
 *     affect the sampler's timing at all)
 *   - if a send takes 25ms, 2-3 frames were built in the meantime;
 *     the sender picks up the LATEST one, not a stale queued one -
 *     exactly right for a joystick (freshest position matters, not
 *     historical completeness)
 *
 * The server prints the timestamp from INSIDE the frame (board's NTP
 * clock at sample time), not the packet arrival time. Since sampling
 * is now perfectly 10ms-spaced regardless of network conditions, the
 * server output will show clean 10ms intervals even when delivery jitters.
 */

/* Shared frame buffer between sampler and sender threads.
 * shared_frame_len is needed alongside the buffer because JSON frames are
 * variable length - both are protected by the same frame_mutex. */
static uint8_t shared_frame[MAX_FRAME_SIZE];
static size_t shared_frame_len;
static struct k_mutex frame_mutex;
static K_SEM_DEFINE(frame_ready, 0, 1);

/* Timer-driven tick for the sampler */
static K_SEM_DEFINE(sample_tick, 0, 1);

static void sample_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&sample_tick);
}

static K_TIMER_DEFINE(sample_timer, sample_timer_handler, NULL);

/* Network sender thread - runs independently, sends latest frame over
 * HTTPS as fast as the network allows. Stack size matches the existing
 * NET_TX_STACK_SIZE since this thread does all the TLS/TCP work. */
#define SENDER_STACK_SIZE 6144
#define SENDER_PRIORITY   7  /* lower priority than main (sampler) so the
			      * sampler always preempts if both are ready.
			      * Higher priority than NTP_RESYNC_PRIORITY (8),
			      * so resync never preempts sending either. */

static void sender_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Block until the sampler signals a new frame is available */
		k_sem_take(&frame_ready, K_FOREVER);

		/* Copy the latest frame under the mutex - hold it as briefly
		 * as possible so the sampler is never blocked waiting on us */
		uint8_t local_frame[MAX_FRAME_SIZE];
		size_t local_frame_len;
		k_mutex_lock(&frame_mutex, K_FOREVER);
		local_frame_len = shared_frame_len;
		memcpy(local_frame, shared_frame, local_frame_len);
		k_mutex_unlock(&frame_mutex);

		/* This is the potentially-slow blocking call. It can take
		 * 5-25ms+ depending on network conditions. Because it runs
		 * on this dedicated thread, not the sampler thread, the
		 * sampler's 10ms timer cadence is completely unaffected. */
		https_conn_send_frame(local_frame, local_frame_len);
	}
}

K_THREAD_STACK_DEFINE(sender_stack, SENDER_STACK_SIZE);

int main(void)
{
#if defined(CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT) || NRF_CLOCK_HAS_HFCLK192M
	/* Run the application core at 128MHz instead of the 64MHz default. */
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
#endif

	k_sleep(K_SECONDS(1));

	if (joystick_init() != 0) {
		LOG_ERR("X/Y input init failed, halting");
		return 0;
	}

	/* Non-fatal on failure - a missing/misconfigured LED shouldn't
	 * halt telemetry, it's a nice-to-have indicator, not core
	 * functionality. */
	if (led_init() != 0) {
		LOG_WRN("Send-activity LED init failed - continuing without it");
	}

	/* Non-fatal on failure: the DK buttons only feed the button_up /
	 * button_down fields of each frame (src/button_inputs.h). A button
	 * that cannot be read simply reports 0 (not pressed); telemetry
	 * itself does not depend on it. */
	if (button_inputs_init() != 0) {
		LOG_WRN("Button state inputs init failed - affected button(s) "
			"will report 0 in every frame");
	}

#ifdef CONFIG_SEND_TRIGGER_NEEDS_BUTTON
	/* FATAL on failure, unlike the LED above, and deliberately so. The
	 * LED is an indicator the board works fine without; this button is
	 * the ONLY thing that can trigger a send. Both accessors fail safe to
	 * "no send" (see src/button.h), so carrying on after a failed init
	 * would leave the board powered, associated, and permanently silent -
	 * a fault that looks identical to a network problem and would cost
	 * real time to diagnose. Halt and say why. */
	if (button_init() != 0) {
		LOG_ERR("Send-trigger button init failed, halting - this build "
			"cannot transmit without it (see SEND_TRIGGER in Kconfig)");
		return 0;
	}
#endif

	/* Calibrate the joystick's true center point now, while it's
	 * expected to still be at rest (before Wi-Fi/TLS setup below, which
	 * gives the person plenty of time to have let go of it if they were
	 * testing it during flashing). See src/joystick.c for why this
	 * replaced a hardcoded theoretical center. */
	joystick_calibrate_center();

	if (https_conn_provision_cert() != 0) {
		LOG_ERR("Cert provisioning failed, halting");
		return 0;
	}

	/* Registers the Wi-Fi/DHCP event callbacks, connects, and blocks
	 * until both Wi-Fi is associated and a DHCP lease is bound (or their
	 * respective timeouts elapse) - see src/wifi_net.c for the full
	 * sequence and the specific error already logged on failure. */
	if (wifi_net_connect_and_wait() != 0) {
		return 0;
	}

	k_sleep(K_SECONDS(3)); /* let association settle before TLS/NTP */

	/* Initial NTP sync happens over plain UDP (no TLS) to the same PC.
	 * If it fails, we log it and carry on - CLOCK_REALTIME will just
	 * stay at epoch 0 (1970-01-01) until the periodic resync thread
	 * (started below) succeeds. */
	if (epoch_sync_now(NTP_SERVER) != 0) {
		LOG_WRN("Continuing without NTP - CLOCK_REALTIME will read epoch-0 "
			"(1970-01-01) until periodic resync succeeds");
	}

	/* Resolves "verdin-imx8mp.local" over mDNS and then opens the TLS +
	 * WebSocket connection. This BLOCKS until both succeed, retrying
	 * every 5 s - see https_conn_open() in src/https_conn.h. The old
	 * "halting" path below can no longer be reached, and is kept only so
	 * that a future bounded variant of https_conn_open() still has a
	 * failure branch here. */
	if (https_conn_open() != 0) {
		LOG_ERR("TLS setup failed, halting");
		return 0;
	}

	/* Prints the ACTUAL period rather than a hardcoded number - replay
	 * builds set it from Kconfig, so a literal here would drift. */
#if defined(CONFIG_SEND_TRIGGER_BUTTON_PRESS)
	LOG_INF("Setup complete - %d ms sampler, ONE FRAME PER PRESS of "
		"Button 1 (two-thread + NTP resync). PRESS Button 1 to send.",
		SEND_PERIOD_MS);
#elif defined(CONFIG_SEND_TRIGGER_BUTTON_HOLD)
	LOG_INF("Setup complete - %d ms X/Y send loop, GATED on Button 1 "
		"(two-thread + NTP resync). HOLD Button 1 to transmit.",
		SEND_PERIOD_MS);
#else
	LOG_INF("Setup complete - entering %d ms X/Y send loop "
		"(two-thread + NTP resync)", SEND_PERIOD_MS);
#endif

	/* Initialise the shared frame mutex */
	k_mutex_init(&frame_mutex);

	/* Start the sender thread BEFORE the timer, so it's ready and
	 * waiting on frame_ready before the first frame is built. */
	static struct k_thread sender_thread_data;
	k_thread_create(&sender_thread_data, sender_stack,
			K_THREAD_STACK_SIZEOF(sender_stack),
			sender_thread_fn, NULL, NULL, NULL,
			SENDER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sender_thread_data, "https_sender");

	/* Start the epoch module's periodic NTP resync thread. Lowest
	 * priority of the three application threads (sampler > sender >
	 * resync), so it can never delay a sample or a send - it only ever
	 * runs when both of those are idle/blocked. */
	epoch_start_resync(NTP_SERVER, NTP_RESYNC_INTERVAL_S, NTP_RESYNC_PRIORITY);

	/* Start the sampling timer */
	k_timer_start(&sample_timer, K_MSEC(SEND_PERIOD_MS), K_MSEC(SEND_PERIOD_MS));

	/* Sampler loop: runs on the main thread, timer-driven, NEVER touches
	 * the network. Reads joystick, builds frame, stores it for the sender
	 * thread to pick up. This loop's timing is completely independent of
	 * network conditions - even if a TCP round trip takes 100ms, this
	 * loop keeps sampling at exactly SEND_PERIOD_MS intervals. */
	while (1) {
		k_sem_take(&sample_tick, K_FOREVER);

		/* Sampled on EVERY tick, gated or not. Two reasons this sits
		 * above the gate rather than below it:
		 *   - joystick.c's current_x_state/current_y_state stay fresh,
		 *     so the first frame after a press carries a reading taken
		 *     at most one tick ago instead of one from whenever the
		 *     button was last released.
		 *   - the ADC debug logging in joystick.c keeps running while
		 *     the gate is shut, which is what makes this build usable
		 *     for the open joystick-wiring investigation.
		 * It costs two ADC reads per tick while idle - cheap, and the
		 * timer is running anyway. */
		joystick_sample();

#ifdef CONFIG_SEND_TRIGGER_NEEDS_BUTTON
		/* ---------------- Send trigger ----------------
		 *
		 * Placed AFTER joystick_sample() and BEFORE frame_build() on
		 * purpose. frame_build() is what increments the sequence
		 * number and (in replay builds) advances the replay row, so
		 * skipping it here PAUSES the trajectory rather than skipping
		 * through it - the recorded pass stays complete and in order
		 * across any number of presses. See SEND_TRIGGER_NEEDS_BUTTON
		 * in Kconfig for the one case where that is still not enough.
		 *
		 * Polled once per tick rather than ISR-driven, and debounced
		 * inside button.c - see src/button.h for why both of those are
		 * the right call for a button that gates transmission. */
		button_poll();
#endif

#if defined(CONFIG_SEND_TRIGGER_BUTTON_PRESS)
		/* ONE frame per press. button_take_press() consumes the latch,
		 * so a held button sends exactly one frame and then nothing
		 * until it is released and pressed again.
		 *
		 * No k_sem_reset() of frame_ready here, unlike the hold mode
		 * below: there is no stream to cut short, and resetting could
		 * discard the single frame this press just asked for. */
		if (!button_take_press()) {
			continue; /* no press this tick - build nothing */
		}

		/* Logged per press. Cheap at human press rates, and it is the
		 * only confirmation the press registered when the resulting
		 * frame fails to send for some unrelated reason. */
		LOG_INF("Button 1 pressed - sending one frame");

#elif defined(CONFIG_SEND_TRIGGER_BUTTON_HOLD)
		/* Stream while held. */
		{
			/* static: must survive across iterations, since the
			 * point is comparing this tick against the last.
			 * Only ever touched by this thread. */
			static bool was_held;
			bool is_held = button_is_held();

			if (is_held != was_held) {
				/* Logged on the EDGE only, never per tick. At
				 * 50 Hz a per-tick line would add ~60 chars of
				 * UART per frame and the console is already at
				 * roughly 100 % utilisation in a replay build. */
				LOG_INF("Send gate %s (Button 1 %s)",
					is_held ? "OPEN - streaming" : "SHUT - idle",
					is_held ? "pressed" : "released");

				if (!is_held) {
					/* Falling edge: drop a frame already
					 * signalled but not yet picked up, so
					 * "release stops the stream" is literally
					 * true rather than true-plus-one-frame.
					 *
					 * A send ALREADY in flight still
					 * completes - the sender thread is
					 * blocked inside zsock_send() and there
					 * is no safe way to unwind that. Expect
					 * at most one more frame after release,
					 * not zero. */
					k_sem_reset(&frame_ready);
				}

				was_held = is_held;
			}

			if (!is_held) {
				continue; /* gate shut - build nothing */
			}
		}
#endif

		uint8_t frame[MAX_FRAME_SIZE];
		size_t frame_len = frame_build(frame);

		/* Store the freshly-built frame in the shared buffer. The
		 * mutex is held for just a memcpy (~nanoseconds), so the
		 * sender thread is never meaningfully blocked by this. */
		k_mutex_lock(&frame_mutex, K_FOREVER);
		memcpy(shared_frame, frame, frame_len);
		shared_frame_len = frame_len;
		k_mutex_unlock(&frame_mutex);

		/* Signal the sender thread that a new frame is available.
		 * k_sem_give with max count 1 means: if the sender hasn't
		 * picked up the previous frame yet (still blocked on the
		 * network), this just overwrites it - the sender will get
		 * the LATEST frame when it's ready, not a stale queued one.
		 * Exactly right for a joystick controller. */
		k_sem_give(&frame_ready);
	}

	return 0;
}
