#include "epoch.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(epoch, CONFIG_LOG_DEFAULT_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/sntp.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define NTP_SYNC_TIMEOUT_MS 5000
#define RESYNC_STACK_SIZE    2048

static bool ntp_ever_synced;

/* Raw NTP server timestamp from the most recent successful sync, captured
 * directly from struct sntp_time as returned by sntp_simple() - NOT
 * derived from CLOCK_REALTIME/epoch_now_ms(). This is the literal value
 * the NTP server sent us, held as-is until the next sync updates it.
 * Protected by ntp_value_mutex since it's written on whichever thread
 * calls epoch_sync_now() (both the caller's initial sync and the resync
 * thread below) and read via epoch_get_last_ntp() from other threads. */
static uint64_t last_ntp_seconds;
static uint32_t last_ntp_fraction;
static K_MUTEX_DEFINE(ntp_value_mutex);

int epoch_sync_now(const char *server_addr)
{
	struct sntp_time ts;
	struct timespec real_ts;
	int ret;

	ret = sntp_simple(server_addr, NTP_SYNC_TIMEOUT_MS, &ts);
	if (ret) {
		LOG_ERR("NTP sync with %s failed (%d) - CLOCK_REALTIME left "
			"unchanged (or still epoch-0 if never synced)",
			server_addr, ret);
		return ret;
	}

	/* Store the raw server timestamp unconditionally right after a
	 * successful sntp_simple() call, before anything else, so it's
	 * exactly what the server sent. */
	k_mutex_lock(&ntp_value_mutex, K_FOREVER);
	last_ntp_seconds = ts.seconds;
	last_ntp_fraction = ts.fraction;
	k_mutex_unlock(&ntp_value_mutex);

	/* ts.seconds is unix epoch seconds (Zephyr's sntp_simple() already
	 * converts from the NTP 1900 epoch). ts.fraction is a Q32 fixed-point
	 * fraction of a second (fraction / 2^32 = fractional seconds) -
	 * convert that to nanoseconds for struct timespec. */
	real_ts.tv_sec  = (time_t)ts.seconds;
	real_ts.tv_nsec = (long)(((uint64_t)ts.fraction * 1000000000ULL) >> 32);

	ret = clock_settime(CLOCK_REALTIME, &real_ts);
	if (ret) {
		LOG_ERR("clock_settime(CLOCK_REALTIME) failed (%d) - "
			"CONFIG_POSIX_CLOCK may not be enabled in prj.conf", ret);
		return ret;
	}

	if (ntp_ever_synced) {
		LOG_INF("NTP resync OK: epoch=%llu.%09ld sec (from %s)",
			(unsigned long long)real_ts.tv_sec, real_ts.tv_nsec,
			server_addr);
	} else {
		LOG_INF("NTP initial sync OK: epoch=%llu.%09ld sec (from %s)",
			(unsigned long long)real_ts.tv_sec, real_ts.tv_nsec,
			server_addr);
		ntp_ever_synced = true;
	}

	return 0;
}

uint64_t epoch_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		/* Should not normally happen; fall back to raw uptime so the
		 * caller never stalls even if this somehow fails. */
		return (uint64_t)k_uptime_get();
	}

	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

void epoch_format(uint64_t epoch_ms, char *buf, size_t buf_len)
{
	time_t sec = (time_t)(epoch_ms / 1000);
	uint32_t ms = (uint32_t)(epoch_ms % 1000);
	struct tm tm_val;

	gmtime_r(&sec, &tm_val);
	snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
		 tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
		 tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, ms);
}

void epoch_get_last_ntp(uint64_t *seconds, uint32_t *fraction)
{
	k_mutex_lock(&ntp_value_mutex, K_FOREVER);
	*seconds = last_ntp_seconds;
	*fraction = last_ntp_fraction;
	k_mutex_unlock(&ntp_value_mutex);
}

/* ---------------- Periodic resync thread ----------------
 * Wakes up every interval_s seconds and re-queries server_addr, pushing
 * the result into CLOCK_REALTIME via epoch_sync_now(). Its only shared
 * state is CLOCK_REALTIME itself (via the kernel's own clock_settime())
 * and ntp_value_mutex above - no coupling to any other application state,
 * so it has no effect on whatever else the caller's other threads do. */
struct resync_args {
	const char *server_addr;
	uint32_t interval_s;
};

static void resync_thread_fn(void *p1, void *p2, void *p3)
{
	const struct resync_args *args = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sleep(K_SECONDS(args->interval_s));

		/* Blocking network I/O happens here, on this dedicated
		 * thread only. Failure just logs and leaves CLOCK_REALTIME
		 * as-is. */
		(void)epoch_sync_now(args->server_addr);
	}
}

K_THREAD_STACK_DEFINE(resync_stack, RESYNC_STACK_SIZE);

void epoch_start_resync(const char *server_addr, uint32_t interval_s, int priority)
{
	static struct k_thread resync_thread_data;
	/* static: the thread reads this for its entire lifetime, so it must
	 * outlive this function call. */
	static struct resync_args args;

	args.server_addr = server_addr;
	args.interval_s = interval_s;

	k_thread_create(&resync_thread_data, resync_stack,
			K_THREAD_STACK_SIZEOF(resync_stack),
			resync_thread_fn, &args, NULL, NULL,
			priority, 0, K_NO_WAIT);
	k_thread_name_set(&resync_thread_data, "epoch_resync");
}
