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
 * Stock QuickRemote dual-drives IR: PWM carrier (IRRC_START) + AudioTrack EWG
 * on LINEOUT1 (mixer path lg-irrc-lineout / MultiMedia2). This HAL best-effort
 * enables the same mixer controls and streams a DC envelope on MultiMedia2
 * while the kernel transmits. Failures are logged; PWM still proceeds.
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
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/consumerir.h>
#include <tinyalsa/asoundlib.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define IRRC_DEVICE "/dev/msm_IRRC_pcm_dec"
#define IRRC_POKE   "/sys/kernel/debug/sw_irrc/poke"

#define IRRC_IOCTL_MAGIC 'a'
#define IRRC_START       _IOW(IRRC_IOCTL_MAGIC, 0, int)
#define IRRC_STOP        _IOW(IRRC_IOCTL_MAGIC, 1, int)

/* Duty cycle accepted by android_irrc_enable_pwm (20..60). */
#define IRRC_DUTY_PERCENT 50

/* msm8226: MultiMedia2 is PCM device 1 (see USECASE_AUDIO_PLAYBACK_MULTI_CH). */
#define IRRC_SND_CARD     0
#define IRRC_PCM_DEVICE   1
#define IRRC_PCM_RATE     48000
#define IRRC_PCM_CHANNELS 1
#define IRRC_PCM_PERIOD_SIZE  1024
#define IRRC_PCM_PERIOD_COUNT 4

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

/* Best-effort LINEOUT envelope (stock lg-irrc-lineout + MultiMedia2). */
struct irrc_lineout_state {
    struct mixer *mixer;
    struct pcm *pcm;
    pthread_t thread;
    int thread_running;
    volatile int run;
    int16_t *buf;
    size_t buf_bytes;
};

struct irrc_mixer_ctl {
    const char *name;
    int is_enum;
    const char *enum_value; /* when is_enum */
    int int_value;          /* when !is_enum */
};

/* Matches configs/mixer_paths.xml paths lg-irrc-playback + lg-irrc-lineout. */
static const struct irrc_mixer_ctl irrc_lineout_on[] = {
    { "SLIMBUS_0_RX Audio Mixer MultiMedia2", 0, NULL, 1 },
    { "SLIM RX1 MUX", 1, "AIF1_PB", 0 },
    { "SLIM_0_RX Channels", 1, "One", 0 },
    { "RX3 MIX1 INP1", 1, "RX1", 0 },
    { "RX3 Digital Volume", 0, NULL, 88 },
    { "LINEOUT1 Volume", 0, NULL, 20 },
    { "SPK DAC Switch", 0, NULL, 0 },
};

static const struct irrc_mixer_ctl irrc_lineout_off[] = {
    { "SLIMBUS_0_RX Audio Mixer MultiMedia2", 0, NULL, 0 },
    { "LINEOUT1 Volume", 0, NULL, 14 },
    { "RX3 Digital Volume", 0, NULL, 84 },
    { "RX3 MIX1 INP1", 1, "ZERO", 0 },
    { "SLIM RX1 MUX", 1, "ZERO", 0 },
};

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

static int irrc_mixer_apply(struct mixer *mixer, const struct irrc_mixer_ctl *ctls,
        size_t n)
{
    size_t i;
    int applied = 0;

    if (!mixer) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mixer, ctls[i].name);
        int rc;

        if (!ctl) {
            ALOGV("mixer ctl missing: %s", ctls[i].name);
            continue;
        }
        if (ctls[i].is_enum) {
            rc = mixer_ctl_set_enum_by_string(ctl, ctls[i].enum_value);
        } else {
            rc = mixer_ctl_set_value(ctl, 0, ctls[i].int_value);
        }
        if (rc != 0) {
            ALOGW("mixer set %s failed (%d)", ctls[i].name, rc);
        } else {
            applied++;
        }
    }
    return applied;
}

static void *irrc_lineout_feed(void *arg)
{
    struct irrc_lineout_state *st = arg;

    prctl(PR_SET_NAME, "irrc-lineout", 0, 0, 0);
    while (st->run && st->pcm) {
        if (pcm_write(st->pcm, st->buf, st->buf_bytes) != 0) {
            ALOGW("irrc LINEOUT pcm_write: %s", pcm_get_error(st->pcm));
            break;
        }
    }
    return NULL;
}

/*
 * Stock QuickRemote: enable lg-irrc-lineout and play an envelope on MultiMedia2
 * while PWM carrier runs. Best-effort — SELinux / busy card must not block IR.
 */
static int irrc_lineout_start(struct irrc_lineout_state *st)
{
    struct pcm_config config;
    unsigned int i;
    int applied;

    memset(st, 0, sizeof(*st));

    st->mixer = mixer_open(IRRC_SND_CARD);
    if (!st->mixer) {
        ALOGW("irrc LINEOUT: mixer_open(%d) failed — PWM-only", IRRC_SND_CARD);
        return -ENODEV;
    }

    applied = irrc_mixer_apply(st->mixer, irrc_lineout_on,
            ARRAY_SIZE(irrc_lineout_on));
    if (applied == 0) {
        ALOGW("irrc LINEOUT: no mixer ctls applied — PWM-only");
        mixer_close(st->mixer);
        st->mixer = NULL;
        return -ENOENT;
    }

    memset(&config, 0, sizeof(config));
    config.channels = IRRC_PCM_CHANNELS;
    config.rate = IRRC_PCM_RATE;
    config.period_size = IRRC_PCM_PERIOD_SIZE;
    config.period_count = IRRC_PCM_PERIOD_COUNT;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = IRRC_PCM_PERIOD_SIZE;
    config.stop_threshold = IRRC_PCM_PERIOD_SIZE * IRRC_PCM_PERIOD_COUNT;
    config.silence_threshold = 0;

    st->pcm = pcm_open(IRRC_SND_CARD, IRRC_PCM_DEVICE, PCM_OUT, &config);
    if (!st->pcm || !pcm_is_ready(st->pcm)) {
        ALOGW("irrc LINEOUT: pcm_open(card=%d,dev=%d) failed (%s) — mixer only",
                IRRC_SND_CARD, IRRC_PCM_DEVICE,
                st->pcm ? pcm_get_error(st->pcm) : "null");
        if (st->pcm) {
            pcm_close(st->pcm);
            st->pcm = NULL;
        }
        /* Mixer path alone can bias LINEOUT amp on some boards. */
        return 0;
    }

    st->buf_bytes = pcm_frames_to_bytes(st->pcm, IRRC_PCM_PERIOD_SIZE);
    st->buf = malloc(st->buf_bytes);
    if (!st->buf) {
        pcm_close(st->pcm);
        st->pcm = NULL;
        return 0;
    }

    /* DC high ≈ stock EWG envelope (full-scale S16). */
    for (i = 0; i < st->buf_bytes / sizeof(int16_t); i++) {
        st->buf[i] = 0x7fff;
    }

    st->run = 1;
    if (pthread_create(&st->thread, NULL, irrc_lineout_feed, st) != 0) {
        ALOGW("irrc LINEOUT: feed thread failed");
        st->run = 0;
        free(st->buf);
        st->buf = NULL;
        pcm_close(st->pcm);
        st->pcm = NULL;
        return 0;
    }
    st->thread_running = 1;
    ALOGI("irrc LINEOUT: MultiMedia2 envelope on (mixer+%d ctls)", applied);
    return 0;
}

static void irrc_lineout_stop(struct irrc_lineout_state *st)
{
    if (!st) {
        return;
    }

    if (st->thread_running) {
        st->run = 0;
        pthread_join(st->thread, NULL);
        st->thread_running = 0;
    }
    if (st->pcm) {
        pcm_close(st->pcm);
        st->pcm = NULL;
    }
    free(st->buf);
    st->buf = NULL;

    if (st->mixer) {
        irrc_mixer_apply(st->mixer, irrc_lineout_off,
                ARRAY_SIZE(irrc_lineout_off));
        mixer_close(st->mixer);
        st->mixer = NULL;
    }
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
    struct irrc_lineout_state lineout;

    if (pattern == NULL || pattern_len <= 0) {
        return -EINVAL;
    }

    {
        char preview[128];
        int n = 0;
        int lim = pattern_len < 12 ? pattern_len : 12;
        int j;

        n += snprintf(preview + n, sizeof(preview) - n, "pattern:");
        for (j = 0; j < lim && n < (int)sizeof(preview) - 1; j++) {
            n += snprintf(preview + n, sizeof(preview) - n, " %d", pattern[j]);
        }
        ALOGI("transmit %d entries at %d Hz; %s",
                pattern_len, carrier_freq, preview);
    }

#ifdef PR_SET_TIMERSLACK
    /* Tighten timer slack before any userspace busy-wait fallback. */
    prctl(PR_SET_TIMERSLACK, 1L);
#endif

    pthread_mutex_lock(&g_lock);

    /* Stock dual path: LINEOUT envelope while PWM/carrier runs. */
    (void)irrc_lineout_start(&lineout);

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
            irrc_lineout_stop(&lineout);
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

    irrc_lineout_stop(&lineout);
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
