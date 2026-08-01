/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Legacy consumerir HAL for LG SwIRRC (android_irrc.c).
 *
 * Prefer IRRC_TRANSMIT: kernel plays the full mark/space pattern with a
 * busy-wait (usleep is insufficient due to timer slack for sub-ms NEC/LG
 * patterns). Falls back to IRRC_START/STOP + userspace busy-wait on older
 * kernels, then to debugfs poke:
 *   echo <enable> <freqKHz> <duty> > /sys/kernel/debug/sw_irrc/poke
 *
 * Kernel IRRC_STOP / poke-off gates carrier immediately (ROOT_EN clear);
 * cakekernel fully disarms clk/rails after 100 ms idle. Stock used a 1500 ms
 * delayed STOP and broke mark/space patterns.
 */

#define LOG_TAG "ConsumerIrHal"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/consumerir.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define IRRC_DEVICE "/dev/msm_IRRC_pcm_dec"
#define IRRC_POKE   "/sys/kernel/debug/sw_irrc/poke"

#define IRRC_IOCTL_MAGIC 'a'
#define IRRC_START       _IOW(IRRC_IOCTL_MAGIC, 0, int)
#define IRRC_STOP        _IOW(IRRC_IOCTL_MAGIC, 1, int)

/* Duty cycle accepted by android_irrc_enable_pwm (20..60). */
#define IRRC_DUTY_PERCENT 50

struct irrc_compr_params {
    int frequency; /* Hz */
    int duty;      /* percent */
    int length;
};

struct irrc_transmit_params {
    int frequency; /* Hz */
    int duty;
    int count;
    const int *pattern; /* userspace pointer; kernel copies */
};

#define IRRC_TRANSMIT _IOW(IRRC_IOCTL_MAGIC, 2, struct irrc_transmit_params)

/*
 * Driver accepts PWM_CLK in kHz for 23..1200; consumer IR is ~20-60 kHz.
 * Report the practical consumer range that fits the driver's checks.
 */
static const consumerir_freq_range_t consumerir_freqs[] = {
    { .min = 23000, .max = 60000 },
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void busy_wait_us(int usec)
{
    struct timespec start, now;
    long long elapsed_ns;
    long long target_ns;

    if (usec <= 0) {
        return;
    }

    target_ns = (long long)usec * 1000LL;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (long long)(now.tv_sec - start.tv_sec) * 1000000000LL
                + (long long)(now.tv_nsec - start.tv_nsec);
    } while (elapsed_ns < target_ns);
}

static int irrc_poke_write(int enable, int freq_khz, int duty)
{
    char buf[64];
    int fd;
    int len;
    int rc;

    fd = open(IRRC_POKE, O_WRONLY);
    if (fd < 0) {
        return -errno;
    }

    len = snprintf(buf, sizeof(buf), "%d %d %d", enable, freq_khz, duty);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        close(fd);
        return -EINVAL;
    }

    rc = write(fd, buf, (size_t)len);
    if (rc < 0) {
        rc = -errno;
        ALOGE("poke write failed: %s", strerror(errno));
    } else {
        rc = 0;
    }
    close(fd);
    return rc;
}

static int irrc_ioctl_start(int fd, int carrier_hz, int duty)
{
    struct irrc_compr_params params;
    int rc;

    memset(&params, 0, sizeof(params));
    params.frequency = carrier_hz;
    params.duty = duty;
    params.length = 0;

    rc = ioctl(fd, IRRC_START, &params);
    if (rc < 0) {
        rc = -errno;
        ALOGE("IRRC_START failed: %s", strerror(errno));
    }
    return rc;
}

static int irrc_ioctl_stop(int fd)
{
    int unused = 0;
    int rc;

    rc = ioctl(fd, IRRC_STOP, &unused);
    if (rc < 0) {
        rc = -errno;
        ALOGE("IRRC_STOP failed: %s", strerror(errno));
    }
    return rc;
}

static int irrc_carrier_on(int fd, int carrier_hz)
{
    int freq_khz = carrier_hz / 1000;

    if (freq_khz < 23) {
        freq_khz = 23;
    }

    if (fd >= 0) {
        return irrc_ioctl_start(fd, carrier_hz, IRRC_DUTY_PERCENT);
    }
    return irrc_poke_write(1, freq_khz, IRRC_DUTY_PERCENT);
}

static int irrc_carrier_off(int fd, int carrier_hz)
{
    int freq_khz = carrier_hz / 1000;

    if (freq_khz < 23) {
        freq_khz = 23;
    }

    if (fd >= 0) {
        return irrc_ioctl_stop(fd);
    }
    return irrc_poke_write(0, freq_khz, IRRC_DUTY_PERCENT);
}

static int consumerir_transmit(struct consumerir_device *dev __unused,
        int carrier_freq, const int pattern[], int pattern_len)
{
    int i;
    int rc = 0;
    int fd = -1;
    struct irrc_transmit_params tx;

    if (pattern == NULL || pattern_len <= 0) {
        return -EINVAL;
    }

    ALOGD("transmit %d entries at %d Hz", pattern_len, carrier_freq);

#ifdef PR_SET_TIMERSLACK
    /* Tighten timer slack before any userspace busy-wait fallback. */
    prctl(PR_SET_TIMERSLACK, 1L);
#endif

    pthread_mutex_lock(&g_lock);

    fd = open(IRRC_DEVICE, O_RDWR);
    if (fd < 0) {
        ALOGW("open %s failed (%s); using poke fallback",
                IRRC_DEVICE, strerror(errno));
    }

    if (fd >= 0) {
        memset(&tx, 0, sizeof(tx));
        tx.frequency = carrier_freq;
        tx.duty = IRRC_DUTY_PERCENT;
        tx.count = pattern_len;
        tx.pattern = pattern;

        rc = ioctl(fd, IRRC_TRANSMIT, &tx);
        if (rc == 0) {
            ALOGD("IRRC_TRANSMIT ok (%d entries @ %d Hz)",
                    pattern_len, carrier_freq);
            close(fd);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }

        ALOGD("IRRC_TRANSMIT unavailable (%s); falling back to START/STOP",
                strerror(errno));
        rc = 0;
    }

    for (i = 0; i < pattern_len; i++) {
        if (pattern[i] < 0) {
            rc = -EINVAL;
            break;
        }

        /* Even index = carrier on, odd = off (ConsumerIrManager contract). */
        if ((i & 1) == 0) {
            rc = irrc_carrier_on(fd, carrier_freq);
        } else {
            rc = irrc_carrier_off(fd, carrier_freq);
        }
        if (rc != 0) {
            break;
        }

        if (pattern[i] > 0) {
            busy_wait_us(pattern[i]);
        }
    }

    /* Always end with carrier off. */
    (void)irrc_carrier_off(fd, carrier_freq);

    if (fd >= 0) {
        close(fd);
    }

    pthread_mutex_unlock(&g_lock);
    return rc;
}

static int consumerir_get_num_carrier_freqs(struct consumerir_device *dev __unused)
{
    return ARRAY_SIZE(consumerir_freqs);
}

static int consumerir_get_carrier_freqs(struct consumerir_device *dev __unused,
        size_t len, consumerir_freq_range_t *ranges)
{
    size_t to_copy = ARRAY_SIZE(consumerir_freqs);

    if (len < to_copy) {
        to_copy = len;
    }
    memcpy(ranges, consumerir_freqs, to_copy * sizeof(consumerir_freq_range_t));
    return (int)to_copy;
}

static int consumerir_close(hw_device_t *dev)
{
    free(dev);
    return 0;
}

static int consumerir_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    consumerir_device_t *dev;

    if (strcmp(name, CONSUMERIR_TRANSMITTER) != 0) {
        return -EINVAL;
    }
    if (device == NULL) {
        ALOGE("NULL device on open");
        return -EINVAL;
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return -ENOMEM;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = consumerir_close;
    dev->transmit = consumerir_transmit;
    dev->get_num_carrier_freqs = consumerir_get_num_carrier_freqs;
    dev->get_carrier_freqs = consumerir_get_carrier_freqs;

    *device = (hw_device_t *)dev;
    return 0;
}

static struct hw_module_methods_t consumerir_module_methods = {
    .open = consumerir_open,
};

consumerir_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CONSUMERIR_MODULE_API_VERSION_1_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CONSUMERIR_HARDWARE_MODULE_ID,
        .name = "LG SwIRRC Consumer IR HAL",
        .author = "The LineageOS Project",
        .methods = &consumerir_module_methods,
    },
};
