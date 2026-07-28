/*
 * Copyright (C) 2015 Sony Mobile Communications Inc.
 * Copyright (C) 2026 The LineageOS v4xx maintainers
 *
 * Derived from Sony TimeKeep. Restore path changed to use
 * /dev/alarm ANDROID_ALARM_SET_RTC so alarm_update_timedelta runs
 * (raw settimeofday leaves ELAPSED_REALTIME stale — issues 001/003).
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names
 * of its contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "TimePersist"
#include <cutils/log.h>
#include <cutils/properties.h>

#ifndef ANDROID_ALARM_SET_RTC
#define ANDROID_ALARM_SET_RTC _IOW('a', 5, struct timespec)
#endif

#define RTC_SYS_FILE "/sys/class/rtc/rtc0/since_epoch"
#define RTC_ATS_FILE "/data/time/ats_2"
#define TIME_ADJUST_PROP "persist.sys.timeadjust"
#define ALARM_DEV "/dev/alarm"

static int read_epoch(unsigned long *epoch)
{
	int fd;
	int res;
	char buffer[16];
	char *endp = NULL;

	fd = open(RTC_SYS_FILE, O_RDONLY);
	if (fd < 0) {
		ALOGW("Failed to open %s: %s", RTC_SYS_FILE, strerror(errno));
		return -errno;
	}

	memset(buffer, 0, sizeof(buffer));
	res = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (res <= 0) {
		ALOGW("Failed to read %s", RTC_SYS_FILE);
		return -1;
	}

	*epoch = strtoul(buffer, &endp, 10);
	if (endp == buffer || (*endp != '\0' && *endp != '\n')) {
		ALOGW("Invalid epoch string from %s: %s", RTC_SYS_FILE, buffer);
		return -1;
	}
	return 0;
}

static void restore_ats(unsigned long adjust_secs)
{
	FILE *fp;
	uint64_t value_ms;
	mode_t mode = 0666;

	mkdir("/data/time", 0771);
	value_ms = (uint64_t)adjust_secs * 1000ULL;
	fp = fopen(RTC_ATS_FILE, "wb");
	if (!fp) {
		ALOGW("Can't write %s: %s", RTC_ATS_FILE, strerror(errno));
		return;
	}
	fwrite(&value_ms, sizeof(value_ms), 1, fp);
	fclose(fp);
	chmod(RTC_ATS_FILE, mode);
}

/*
 * Set wall clock via /dev/alarm so kernel alarm_set_rtc() updates the
 * ELAPSED_REALTIME delta. On this PMIC, rtc_set_time() often returns EPERM
 * after delta+settimeofday already succeeded — treat that as success if the
 * wall clock matches the requested time.
 */
static int set_wall_clock_via_alarm(time_t secs)
{
	struct timespec ts;
	struct timeval tv_after;
	int fd;
	int res;

	fd = open(ALARM_DEV, O_RDWR);
	if (fd < 0) {
		ALOGW("Failed to open %s: %s", ALARM_DEV, strerror(errno));
		return -1;
	}

	ts.tv_sec = secs;
	ts.tv_nsec = 0;
	res = ioctl(fd, ANDROID_ALARM_SET_RTC, &ts);
	close(fd);

	if (res == 0)
		return 0;

	/* RTC chip write often fails with EPERM on locked qpnp RTC. */
	if (gettimeofday(&tv_after, NULL) == 0) {
		long delta = (long)tv_after.tv_sec - (long)secs;
		if (delta < 0)
			delta = -delta;
		if (delta <= 2) {
			ALOGI("ANDROID_ALARM_SET_RTC returned %s but wall clock ok",
			      strerror(errno));
			return 0;
		}
	}

	ALOGW("ANDROID_ALARM_SET_RTC failed: %s", strerror(errno));
	return -1;
}

static int store_time(void)
{
	char prop[PROPERTY_VALUE_MAX];
	unsigned long epoch_since = 0;
	time_t now;
	unsigned long adjust;

	now = time(NULL);
	if (now <= 0) {
		ALOGW("time() failed while storing");
		return -1;
	}

	if (read_epoch(&epoch_since) < 0)
		return -1;

	adjust = (unsigned long)now - epoch_since;
	snprintf(prop, PROPERTY_VALUE_MAX, "%lu", adjust);
	restore_ats(adjust);
	property_set(TIME_ADJUST_PROP, prop);
	ALOGI("Time adjustment stored (%lu)", adjust);
	return 0;
}

static int restore_time(void)
{
	char prop[PROPERTY_VALUE_MAX];
	unsigned long time_adjust = 0;
	unsigned long epoch_since = 0;
	char *endp = NULL;
	time_t target;

	memset(prop, 0, sizeof(prop));
	property_get(TIME_ADJUST_PROP, prop, "0");
	if (strcmp(prop, "0") == 0) {
		ALOGI("No time adjust value found for restore");
		return -1;
	}

	time_adjust = strtoul(prop, &endp, 10);
	if (endp == prop || *endp != '\0') {
		ALOGW("Invalid %s: %s", TIME_ADJUST_PROP, prop);
		return -1;
	}

	if (read_epoch(&epoch_since) < 0)
		return -1;

	restore_ats(time_adjust);
	target = (time_t)(epoch_since + time_adjust);

	if (set_wall_clock_via_alarm(target) != 0)
		return -1;

	ALOGI("Time restored via ANDROID_ALARM_SET_RTC!");
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		ALOGI("usage: timepersist store|restore");
		return -1;
	}

	if (strcmp(argv[1], "store") == 0)
		return store_time();
	if (strcmp(argv[1], "restore") == 0)
		return restore_time();

	ALOGI("usage: timepersist store|restore");
	return -1;
}
