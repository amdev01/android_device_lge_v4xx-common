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
 * Transmit model (proven HITL 2026-11-08, TV powers on):
 *
 *   1. Enable mixer path lg-irrc-playback + lg-irrc-lineout
 *      (MultiMedia2 -> SLIM RX1 -> RX3 -> RDAC4/DEM3 -> LINEOUT1).
 *   2. Start a CONTINUOUS PWM carrier at 30% duty
 *      (IRRC_START, fallback: echo 1 <kHz> 30 > .../sw_irrc/poke).
 *   3. Play the whole mark/space pattern as a PCM *envelope* on MultiMedia2
 *      (card0 dev1, 48 kHz S16LE mono): mark = 0x7FFF, space = 0x0000,
 *      preceded by ~40 ms of silence (DAPM settle) so the codec / LINEOUT PA
 *      are up before the first mark. The stream is kept as short as possible:
 *      lead + pattern + one period of tail, padded to a period boundary.
 *   4. Stop the carrier (IRRC_STOP / poke off), close PCM, restore mixer.
 *
 * Fail-fast: MultiMedia2 can be held by AudioFlinger / contended, in which
 * case a blocking pcm_write() stalls ~10 s and then fails with EIO while
 * g_lock is held. We therefore only call pcm_write() when the ring has room
 * (pcm_get_htimestamp() avail), with a stall / total time budget, and abort
 * with -EIO + full teardown when the budget is exceeded.
 *
 * The LINEOUT amplitude envelope gates the continuous carrier. The earlier
 * approach (DC 0x7FFF on LINEOUT + kernel IRRC_TRANSMIT gated carrier) only
 * made the emitter glow on a camera and was ignored by the TV.
 *
 * IRRC_TRANSMIT (kernel busy-wait) is kept ONLY as a last resort when the PCM
 * envelope path cannot be set up at all (mixer / pcm_open / carrier start).
 *
 * Grep logcat for "EWG PCM envelope" to follow the primary path.
 */

#define LOG_TAG "ConsumerIrHal"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

/* Proven duty for the EWG envelope path (driver accepts 20..60). */
#define IRRC_DUTY_PERCENT 30

/* msm8226: MultiMedia2 is PCM device 1 (see USECASE_AUDIO_PLAYBACK_MULTI_CH). */
#define IRRC_SND_CARD         0
#define IRRC_PCM_DEVICE       1
#define IRRC_PCM_RATE         48000
#define IRRC_PCM_CHANNELS     1
#define IRRC_PCM_PERIOD_SIZE  1024
#define IRRC_PCM_PERIOD_COUNT 4

#define IRRC_PCM_MARK  ((int16_t)0x7fff)
#define IRRC_PCM_SPACE ((int16_t)0x0000)

/* Leading silence (DAPM settle, proven in older HAL): 40 ms = 1920 frames. */
#define IRRC_LEAD_SILENCE_MS 40
#define IRRC_LEAD_FRAMES ((IRRC_PCM_RATE / 1000) * IRRC_LEAD_SILENCE_MS)

/*
 * Trailing silence: one period only. The writer waits for the ring to drain
 * past the pattern body before close (see irrc_pcm_drain_wait), so a full
 * ring of padding is not needed.
 */
#define IRRC_TAIL_FRAMES IRRC_PCM_PERIOD_SIZE

/* Reject patterns that would need more than ~1 s of PCM (48000 frames, ~94 KiB). */
#define IRRC_MAX_PCM_FRAMES (IRRC_PCM_RATE * 1)

/*
 * Fail-fast budgets (ms). A healthy stream drains one period (~21 ms) at a
 * time; if the ring makes no progress for IRRC_STALL_NOPROGRESS_MS we give up.
 * The whole stream may additionally take at most its own playback duration
 * plus IRRC_STALL_BUDGET_MS.
 */
#define IRRC_STALL_NOPROGRESS_MS 200
#define IRRC_STALL_BUDGET_MS     500
#define IRRC_POLL_SLEEP_US       2000
/* Extra settle after the ring drained, before carrier/PCM teardown. */
#define IRRC_POST_DRAIN_MS       10
/* Delay before the single fresh-pcm_open retry. */
#define IRRC_RETRY_DELAY_MS      50

/*
 * tinyalsa PCM_NONBLOCK (0x10 upstream) is NOT used: the old tinyalsa
 * pcm_write() ignores xfer.result, so a partial non-blocking write would
 * silently drop/misalign samples. We gate writes on pcm_get_htimestamp()
 * avail instead, which never blocks and works with or without NONBLOCK.
 */

/* Sanity cap on pattern entries (real remotes use < ~600). */
#define IRRC_MAX_PATTERN_ENTRIES 4096

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

struct irrc_mixer_ctl {
    const char *name;
    int is_enum;
    const char *enum_value; /* when is_enum */
    int int_value;          /* when !is_enum */
};

/*
 * Matches configs/mixer_paths.xml lg-irrc-playback + lg-irrc-lineout.
 * RDAC4->DEM3 is required so LINEOUT1 DAC sees RX3.
 *
 * NEVER set "SLIM RX1 MUX" to "ZERO" on teardown (HITL 2026-11-08): that
 * empties AIF1_PB's SLIM channel list in wcd9306; AudioFlinger then wedges
 * MultiMedia2/speaker (pcm_write 10s EIO, AF write blocked 20-45s). Leave
 * SLIM RX1 MUX at AIF1_PB (boot default). Disconnect LINEOUT via RDAC4->DEM2.
 */
static const struct irrc_mixer_ctl irrc_lineout_on[] = {
    { "SLIMBUS_0_RX Audio Mixer MultiMedia2", 0, NULL, 1 },
    { "SLIM RX1 MUX", 1, "AIF1_PB", 0 },
    { "SLIM_0_RX Channels", 1, "One", 0 },
    { "RX3 MIX1 INP1", 1, "RX1", 0 },
    { "RX3 Digital Volume", 0, NULL, 88 },
    { "RDAC4 MUX", 1, "DEM3", 0 },
    { "LINEOUT1 Volume", 0, NULL, 20 },
    { "SPK DAC Switch", 0, NULL, 0 },
};

/* Idle teardown when speaker was not active before our blast. */
static const struct irrc_mixer_ctl irrc_lineout_off_idle[] = {
    { "SLIMBUS_0_RX Audio Mixer MultiMedia2", 0, NULL, 0 },
    { "RDAC4 MUX", 1, "DEM2", 0 },
    { "LINEOUT1 Volume", 0, NULL, 14 },
    { "RX3 Digital Volume", 0, NULL, 84 },
    { "RX3 MIX1 INP1", 1, "ZERO", 0 },
};

struct irrc_saved_mixer {
    int spk_dac;
    int rx3_vol;
    int lineout_vol;
    int rx3_inp1;
    int valid;
};

/* Carrier handle: either an open IRRC fd (IRRC_START) or poke-only (fd < 0). */
struct irrc_carrier {
    int fd;
    int freq_khz;
    int active;
};

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

/*
 * Snapshot the speaker-path ctls we mutate so teardown can restore AF's path
 * instead of forcing ZERO on shared SLIM RX1 (which breaks MultiMedia2).
 */
static void irrc_mixer_save(struct mixer *mixer, struct irrc_saved_mixer *s)
{
    struct mixer_ctl *ctl;

    memset(s, 0, sizeof(*s));
    if (!mixer) {
        return;
    }

    ctl = mixer_get_ctl_by_name(mixer, "SPK DAC Switch");
    if (ctl) {
        s->spk_dac = mixer_ctl_get_value(ctl, 0);
    }
    ctl = mixer_get_ctl_by_name(mixer, "RX3 Digital Volume");
    if (ctl) {
        s->rx3_vol = mixer_ctl_get_value(ctl, 0);
    }
    ctl = mixer_get_ctl_by_name(mixer, "LINEOUT1 Volume");
    if (ctl) {
        s->lineout_vol = mixer_ctl_get_value(ctl, 0);
    }
    ctl = mixer_get_ctl_by_name(mixer, "RX3 MIX1 INP1");
    if (ctl) {
        s->rx3_inp1 = mixer_ctl_get_value(ctl, 0);
    }
    s->valid = 1;
}

static void irrc_mixer_restore(struct mixer *mixer, const struct irrc_saved_mixer *s)
{
    struct mixer_ctl *ctl;

    if (!mixer || !s || !s->valid) {
        return;
    }

    /* Always disconnect LINEOUT from DEM3 and drop MultiMedia2 feed. */
    ctl = mixer_get_ctl_by_name(mixer, "SLIMBUS_0_RX Audio Mixer MultiMedia2");
    if (ctl) {
        mixer_ctl_set_value(ctl, 0, 0);
    }
    ctl = mixer_get_ctl_by_name(mixer, "RDAC4 MUX");
    if (ctl) {
        mixer_ctl_set_enum_by_string(ctl, "DEM2");
    }

    /* Restore AF speaker path; do NOT touch SLIM RX1 MUX. */
    ctl = mixer_get_ctl_by_name(mixer, "SPK DAC Switch");
    if (ctl) {
        mixer_ctl_set_value(ctl, 0, s->spk_dac);
    }
    ctl = mixer_get_ctl_by_name(mixer, "RX3 Digital Volume");
    if (ctl) {
        mixer_ctl_set_value(ctl, 0, s->rx3_vol);
    }
    ctl = mixer_get_ctl_by_name(mixer, "LINEOUT1 Volume");
    if (ctl) {
        mixer_ctl_set_value(ctl, 0, s->lineout_vol);
    }
    ctl = mixer_get_ctl_by_name(mixer, "RX3 MIX1 INP1");
    if (ctl) {
        mixer_ctl_set_value(ctl, 0, s->rx3_inp1);
    }
}

static void irrc_mixer_teardown(struct mixer *mixer, const struct irrc_saved_mixer *s)
{
    if (!mixer) {
        return;
    }
    if (s && s->valid) {
        irrc_mixer_restore(mixer, s);
    } else {
        irrc_mixer_apply(mixer, irrc_lineout_off_idle,
                ARRAY_SIZE(irrc_lineout_off_idle));
    }
}

static int irrc_poke_write(int enable, int freq_khz, int duty)
{
    char buf[64];
    int fd;
    int len;
    ssize_t wr;
    int rc = 0;

    fd = open(IRRC_POKE, O_WRONLY);
    if (fd < 0) {
        return -errno;
    }

    len = snprintf(buf, sizeof(buf), "%d %d %d", enable, freq_khz, duty);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        close(fd);
        return -EINVAL;
    }

    wr = write(fd, buf, (size_t)len);
    if (wr < 0) {
        rc = -errno;
        ALOGE("poke write failed: %s", strerror(errno));
    }
    close(fd);
    return rc;
}

static int irrc_ioctl_start(int fd, int carrier_hz, int duty)
{
    struct irrc_compr_params params;

    memset(&params, 0, sizeof(params));
    params.frequency = carrier_hz;
    params.duty = duty;
    params.length = 0;

    if (ioctl(fd, IRRC_START, &params) < 0) {
        int err = errno;
        ALOGW("IRRC_START failed: %s", strerror(err));
        return -err;
    }
    return 0;
}

static int irrc_ioctl_stop(int fd)
{
    int unused = 0;

    if (ioctl(fd, IRRC_STOP, &unused) < 0) {
        int err = errno;
        ALOGW("IRRC_STOP failed: %s", strerror(err));
        return -err;
    }
    return 0;
}

/*
 * Start continuous carrier. Prefer IRRC_START; fall back to debugfs poke
 * ("echo 1 38 30 > poke"), the form proven in HITL.
 */
static int irrc_carrier_start(struct irrc_carrier *c, int carrier_hz)
{
    int rc;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->freq_khz = (carrier_hz + 500) / 1000;
    if (c->freq_khz < 23) {
        c->freq_khz = 23;
    }

    c->fd = open(IRRC_DEVICE, O_RDWR);
    if (c->fd >= 0) {
        rc = irrc_ioctl_start(c->fd, carrier_hz, IRRC_DUTY_PERCENT);
        if (rc == 0) {
            c->active = 1;
            ALOGI("EWG PCM envelope: carrier IRRC_START %d Hz duty %d%%",
                    carrier_hz, IRRC_DUTY_PERCENT);
            return 0;
        }
        close(c->fd);
        c->fd = -1;
    } else {
        ALOGW("open %s failed (%s); trying poke", IRRC_DEVICE, strerror(errno));
    }

    rc = irrc_poke_write(1, c->freq_khz, IRRC_DUTY_PERCENT);
    if (rc == 0) {
        c->active = 1;
        ALOGI("EWG PCM envelope: carrier poke \"1 %d %d\"",
                c->freq_khz, IRRC_DUTY_PERCENT);
        return 0;
    }

    ALOGE("EWG PCM envelope: carrier start failed (poke %s)", strerror(-rc));
    return rc;
}

static void irrc_carrier_stop(struct irrc_carrier *c)
{
    if (!c->active) {
        return;
    }

    if (c->fd >= 0) {
        if (irrc_ioctl_stop(c->fd) != 0) {
            /* Belt and braces: make sure the carrier really is off. */
            (void)irrc_poke_write(0, c->freq_khz, IRRC_DUTY_PERCENT);
        }
        close(c->fd);
        c->fd = -1;
    } else {
        (void)irrc_poke_write(0, c->freq_khz, IRRC_DUTY_PERCENT);
    }
    c->active = 0;
    ALOGI("EWG PCM envelope: carrier off");
}

/*
 * Some apps (notably us.spotco.ir_remote) hand Pronto *carrier-cycle* counts
 * to ConsumerIrManager, which documents microseconds. At ~38 kHz that makes
 * frames ~26x too short (headers look like 170/342 instead of 4500/9000).
 *
 * Detect Pronto-scale patterns and convert: us = cycles * 1e6 / freq.
 */
static int irrc_pattern_looks_pronto_cycles(int carrier_freq,
        const int pattern[], int pattern_len)
{
    int i;
    int max = 0;
    int header;

    if (carrier_freq < 20000 || pattern_len < 8) {
        return 0;
    }

    for (i = 0; i < pattern_len; i++) {
        if (pattern[i] < 0) {
            return 0;
        }
        if (pattern[i] > max) {
            max = pattern[i];
        }
    }

    /* Real NEC/Samsung us frames have headers >= ~4000. Pronto stays < ~1000. */
    if (max >= 1000) {
        return 0;
    }

    header = pattern[0];
    /* Classic Pronto headers at 38 kHz: NEC~342, Samsung NECx~170-171. */
    if (header >= 150 && header <= 400) {
        return 1;
    }

    /* Fallback: dense short pattern with tiny bit times (e.g. 21). */
    return (max < 500 && pattern_len >= 48);
}

static int irrc_convert_pronto_to_us(int carrier_freq, const int *in,
        int *out, int n)
{
    int i;

    if (carrier_freq <= 0) {
        return -EINVAL;
    }

    for (i = 0; i < n; i++) {
        /* Round nearest: (cycles * 1e6 + freq/2) / freq */
        out[i] = (int)(((long long)in[i] * 1000000LL
                + carrier_freq / 2) / carrier_freq);
    }
    return 0;
}

/* Convert a cumulative time in microseconds to a PCM frame index (rounded). */
static long long irrc_us_to_frames(long long us)
{
    return (us * IRRC_PCM_RATE + 500000LL) / 1000000LL;
}

/*
 * Synthesize the PCM envelope: [40 ms silence][pattern][1-period tail], padded
 * only up to the next period boundary. Even pattern index = mark (0x7FFF), odd =
 * space (0). Boundaries are derived from the cumulative time so rounding error
 * does not accumulate across entries.
 *
 * Returns a calloc'd buffer (caller frees) or NULL with *err set.
 */
static int16_t *irrc_synth_envelope(const int *pattern_us, int pattern_len,
        size_t *out_frames, int *err)
{
    const long long max_body_us = (long long)IRRC_MAX_PCM_FRAMES * 1000000LL
            / IRRC_PCM_RATE;
    long long cum_us = 0;
    long long body_frames;
    size_t total;
    size_t i;
    long long prev_end = 0;
    int16_t *buf;
    int k;

    for (k = 0; k < pattern_len; k++) {
        if (pattern_us[k] < 0) {
            ALOGE("EWG PCM envelope: negative duration at index %d", k);
            *err = -EINVAL;
            return NULL;
        }
        cum_us += pattern_us[k];
        if (cum_us > max_body_us) {
            ALOGE("EWG PCM envelope: pattern too long (> %lld us at idx %d)",
                    max_body_us, k);
            *err = -EINVAL;
            return NULL;
        }
    }

    body_frames = irrc_us_to_frames(cum_us);
    total = (size_t)IRRC_LEAD_FRAMES + (size_t)body_frames
            + (size_t)IRRC_TAIL_FRAMES;
    /* Pad to whole periods. */
    total = ((total + IRRC_PCM_PERIOD_SIZE - 1) / IRRC_PCM_PERIOD_SIZE)
            * IRRC_PCM_PERIOD_SIZE;

    if (total > (size_t)IRRC_MAX_PCM_FRAMES) {
        ALOGE("EWG PCM envelope: %zu frames exceeds cap %d (~2 s)",
                total, IRRC_MAX_PCM_FRAMES);
        *err = -EINVAL;
        return NULL;
    }

    buf = calloc(total, sizeof(int16_t)); /* zero == space */
    if (!buf) {
        ALOGE("EWG PCM envelope: calloc(%zu frames) failed", total);
        *err = -ENOMEM;
        return NULL;
    }

    cum_us = 0;
    for (i = 0; i < (size_t)pattern_len; i++) {
        long long end;

        cum_us += pattern_us[i];
        end = irrc_us_to_frames(cum_us);
        if (end > body_frames) {
            end = body_frames;
        }
        if ((i & 1) == 0 && end > prev_end) {
            long long f;
            for (f = prev_end; f < end; f++) {
                buf[(size_t)IRRC_LEAD_FRAMES + (size_t)f] = IRRC_PCM_MARK;
            }
        }
        prev_end = end;
    }

    *out_frames = total;
    return buf;
}

static long long irrc_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void irrc_msleep(int ms)
{
    usleep((useconds_t)ms * 1000);
}

static int irrc_pcm_open(struct pcm **out)
{
    struct pcm_config config;
    struct pcm *pcm;

    memset(&config, 0, sizeof(config));
    config.channels = IRRC_PCM_CHANNELS;
    config.rate = IRRC_PCM_RATE;
    config.period_size = IRRC_PCM_PERIOD_SIZE;
    config.period_count = IRRC_PCM_PERIOD_COUNT;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = IRRC_PCM_PERIOD_SIZE;
    config.stop_threshold = IRRC_PCM_PERIOD_SIZE * IRRC_PCM_PERIOD_COUNT;
    config.silence_threshold = 0;

    pcm = pcm_open(IRRC_SND_CARD, IRRC_PCM_DEVICE, PCM_OUT, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        ALOGW("EWG PCM envelope: pcm_open(card=%d,dev=%d) failed (%s)",
                IRRC_SND_CARD, IRRC_PCM_DEVICE,
                pcm ? pcm_get_error(pcm) : "null");
        if (pcm) {
            pcm_close(pcm);
        }
        return -EIO;
    }
    *out = pcm;
    return 0;
}

/*
 * Stream the envelope without ever sitting in a blocking pcm_write().
 *
 * The first ring-buffer worth of frames is written blind (nothing has been
 * consumed yet, so it cannot block). After that a period is only written when
 * pcm_get_htimestamp() reports >= one period of space; otherwise we poll and
 * abort on IRRC_STALL_NOPROGRESS_MS without ring progress or when the total
 * time exceeds the stream duration + IRRC_STALL_BUDGET_MS.
 *
 * *written_out = frames handed to pcm_write so far.
 * Returns 0 or -EIO (stall / pcm_write error; caller tears everything down).
 */
static int irrc_pcm_stream(struct pcm *pcm, const int16_t *samples,
        size_t total_frames, size_t *written_out)
{
    const long long t0 = irrc_now_ms();
    const long long stream_ms = (long long)total_frames * 1000LL
            / IRRC_PCM_RATE;
    const long long deadline = t0 + stream_ms + IRRC_STALL_BUDGET_MS;
    unsigned int buffer_frames = pcm_get_buffer_size(pcm);
    unsigned int avail = 0;
    unsigned int last_avail = 0;
    long long last_progress = t0;
    int have_avail = 1;
    int have_last_avail = 0;
    size_t done = 0;

    if (buffer_frames == 0) {
        buffer_frames = IRRC_PCM_PERIOD_SIZE * IRRC_PCM_PERIOD_COUNT;
    }

    *written_out = 0;

    while (done < total_frames) {
        size_t chunk = total_frames - done;
        unsigned int bytes;
        long long now;
        int rc;

        if (chunk > IRRC_PCM_PERIOD_SIZE) {
            chunk = IRRC_PCM_PERIOD_SIZE;
        }

        /* Gate on ring space once the initial (empty) ring has been filled. */
        if (have_avail && done + chunk > buffer_frames) {
            struct timespec ts;

            if (pcm_get_htimestamp(pcm, &avail, &ts) != 0) {
                ALOGW("EWG PCM envelope: pcm_get_htimestamp failed; "
                        "falling back to plain pcm_write");
                have_avail = 0;
                continue;
            }
            now = irrc_now_ms();
            if (!have_last_avail || avail != last_avail) {
                last_avail = avail;
                have_last_avail = 1;
                last_progress = now;
            }
            if (avail < chunk) {
                if (now - last_progress > IRRC_STALL_NOPROGRESS_MS
                        || now > deadline) {
                    ALOGE("EWG PCM envelope: pcm_write timeout/stall at frame "
                            "%zu/%zu after %lld ms (avail=%u, no progress "
                            "%lld ms)", done, total_frames, now - t0, avail,
                            now - last_progress);
                    *written_out = done;
                    return -EIO;
                }
                usleep(IRRC_POLL_SLEEP_US);
                continue;
            }
        }

        bytes = pcm_frames_to_bytes(pcm, (unsigned int)chunk);
        rc = pcm_write(pcm, samples + done * IRRC_PCM_CHANNELS, bytes);
        now = irrc_now_ms();
        if (rc != 0) {
            ALOGE("EWG PCM envelope: pcm_write failed at frame %zu/%zu after "
                    "%lld ms: %s", done, total_frames, now - t0,
                    pcm_get_error(pcm));
            *written_out = done;
            return -EIO;
        }
        done += chunk;
        *written_out = done;
        last_progress = now;
        have_last_avail = 0;

        if (!have_avail && now > deadline) {
            /* No way to poll; a slow blocking write already ate the budget. */
            ALOGE("EWG PCM envelope: pcm_write timeout/stall at frame %zu/%zu "
                    "after %lld ms (blocking path)", done, total_frames,
                    now - t0);
            return -EIO;
        }
    }

    /*
     * Everything is queued. Closing the PCM now would drop what is still in
     * the ring, so wait until the pattern body has been consumed (queued
     * frames <= tail). Same fail-fast budget applies.
     */
    if (have_avail) {
        struct timespec ts;

        have_last_avail = 0;
        last_progress = irrc_now_ms();
        for (;;) {
            long long now;
            unsigned int queued;

            if (pcm_get_htimestamp(pcm, &avail, &ts) != 0) {
                have_avail = 0;
                break;
            }
            queued = avail >= buffer_frames ? 0 : buffer_frames - avail;
            if (queued <= IRRC_TAIL_FRAMES) {
                break;
            }
            now = irrc_now_ms();
            if (!have_last_avail || avail != last_avail) {
                last_avail = avail;
                have_last_avail = 1;
                last_progress = now;
            }
            if (now - last_progress > IRRC_STALL_NOPROGRESS_MS
                    || now > deadline) {
                ALOGE("EWG PCM envelope: pcm_write timeout/stall draining "
                        "ring after %lld ms (queued=%u frames)", now - t0,
                        queued);
                return -EIO;
            }
            usleep(IRRC_POLL_SLEEP_US);
        }
    }
    if (!have_avail) {
        /* Cannot observe the ring: sleep for what could still be queued. */
        size_t q = total_frames < buffer_frames ? total_frames : buffer_frames;

        irrc_msleep((int)(q * 1000 / IRRC_PCM_RATE));
    }
    irrc_msleep(IRRC_POST_DRAIN_MS);

    ALOGI("EWG PCM envelope: streamed %zu frames in %lld ms",
            total_frames, irrc_now_ms() - t0);
    return 0;
}

/*
 * Primary path. Returns:
 *    0          envelope transmitted
 *   -EAGAIN     setup failed before any IR was emitted (caller may fall back)
 *   other <0    hard failure (invalid pattern, alloc, stall / pcm_write error)
 */
static int irrc_transmit_ewg_pcm(int carrier_freq, const int *pattern_us,
        int pattern_len)
{
    struct mixer *mixer = NULL;
    struct pcm *pcm = NULL;
    struct irrc_carrier carrier;
    struct irrc_saved_mixer saved;
    int16_t *samples = NULL;
    size_t total_frames = 0;
    size_t written = 0;
    long long t_start;
    int applied;
    int attempt;
    int err = 0;
    int rc = 0;
    int setup_failed = 0;

    memset(&carrier, 0, sizeof(carrier));
    carrier.fd = -1;
    memset(&saved, 0, sizeof(saved));

    samples = irrc_synth_envelope(pattern_us, pattern_len, &total_frames, &err);
    if (!samples) {
        /* Invalid / too long / OOM: not a hardware-path issue. */
        return err ? err : -ENOMEM;
    }

    t_start = irrc_now_ms();
    ALOGI("EWG PCM envelope: %d entries -> %zu frames (~%zu ms, %d ms lead, "
            "%d Hz, mark=0x%04x space=0x%04x) carrier %d Hz duty %d%%",
            pattern_len, total_frames,
            total_frames * 1000 / IRRC_PCM_RATE, IRRC_LEAD_SILENCE_MS,
            IRRC_PCM_RATE,
            (unsigned)(uint16_t)IRRC_PCM_MARK,
            (unsigned)(uint16_t)IRRC_PCM_SPACE,
            carrier_freq, IRRC_DUTY_PERCENT);

    mixer = mixer_open(IRRC_SND_CARD);
    if (!mixer) {
        ALOGW("EWG PCM envelope: mixer_open(%d) failed", IRRC_SND_CARD);
        setup_failed = 1;
        goto out;
    }

    irrc_mixer_save(mixer, &saved);

    applied = irrc_mixer_apply(mixer, irrc_lineout_on,
            ARRAY_SIZE(irrc_lineout_on));
    if (applied == 0) {
        ALOGW("EWG PCM envelope: no mixer ctls applied");
        setup_failed = 1;
        goto out;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        rc = irrc_pcm_open(&pcm);
        if (rc != 0) {
            /* First open failing = path unavailable (fallback ok);
             * retry open failing = hard failure. */
            if (attempt == 0) {
                setup_failed = 1;
            } else {
                rc = -EIO;
            }
            goto out;
        }

        /* Continuous carrier first; the envelope on LINEOUT gates it. */
        rc = irrc_carrier_start(&carrier, carrier_freq);
        if (rc != 0) {
            if (attempt == 0) {
                setup_failed = 1;
            } else {
                rc = -EIO;
            }
            goto out;
        }

        ALOGI("EWG PCM envelope: streaming%s (mixer ctls=%d)",
                attempt ? " [retry]" : "", applied);

        written = 0;
        rc = irrc_pcm_stream(pcm, samples, total_frames, &written);
        if (rc == 0) {
            break;
        }

        /* Failure: carrier off + PCM closed before anything else. */
        irrc_carrier_stop(&carrier);
        pcm_close(pcm);
        pcm = NULL;

        /*
         * Retry once with a fresh pcm_open only if no pattern body has been
         * queued yet (at most the first period of lead silence), so the TV
         * can never see a duplicated / truncated frame.
         */
        if (attempt == 0 && written <= IRRC_LEAD_FRAMES) {
            ALOGW("EWG PCM envelope: early failure (%zu frames written); "
                    "retrying once in %d ms", written, IRRC_RETRY_DELAY_MS);
            irrc_msleep(IRRC_RETRY_DELAY_MS);
            continue;
        }
        rc = -EIO;
        break;
    }

    if (rc == 0) {
        ALOGI("EWG PCM envelope: done (%zu frames, %lld ms total)",
                total_frames, irrc_now_ms() - t_start);
    } else {
        ALOGE("EWG PCM envelope: failed rc=%d after %lld ms total",
                rc, irrc_now_ms() - t_start);
    }

out:
    /* Always: carrier off, then PCM, then AF-safe mixer restore. */
    irrc_carrier_stop(&carrier);
    if (pcm) {
        pcm_close(pcm);
    }
    if (mixer) {
        irrc_mixer_teardown(mixer, &saved);
        mixer_close(mixer);
    }
    free(samples);

    if (setup_failed) {
        return -EAGAIN;
    }
    return rc;
}

/*
 * Last resort only: kernel IRRC_TRANSMIT (gated carrier, no EWG envelope).
 * HITL showed this does not drive the TV on its own, but it is better than
 * silently dropping the request if audio routing is unavailable.
 */
static int irrc_transmit_kernel_fallback(int carrier_freq, const int *pattern_us,
        int pattern_len)
{
    struct irrc_transmit_params tx;
    int fd;
    int rc;

    fd = open(IRRC_DEVICE, O_RDWR);
    if (fd < 0) {
        int err = errno;
        ALOGE("last-resort IRRC_TRANSMIT: open %s failed: %s",
                IRRC_DEVICE, strerror(err));
        return -err;
    }

    memset(&tx, 0, sizeof(tx));
    tx.frequency = carrier_freq;
    tx.duty = IRRC_DUTY_PERCENT;
    tx.count = pattern_len;
    tx.pattern = pattern_us;

    rc = ioctl(fd, IRRC_TRANSMIT, &tx);
    if (rc < 0) {
        rc = -errno;
        ALOGE("last-resort IRRC_TRANSMIT failed: %s", strerror(-rc));
    } else {
        ALOGW("last-resort IRRC_TRANSMIT ok (%d entries @ %d Hz) - "
                "EWG PCM envelope was NOT used", pattern_len, carrier_freq);
    }
    close(fd);
    return rc;
}

static int consumerir_transmit(struct consumerir_device *dev __unused,
        int carrier_freq, const int pattern[], int pattern_len)
{
    int rc;
    int *pattern_us = NULL;
    const int *xmit_pattern = pattern;
    int converted = 0;

    if (pattern == NULL || pattern_len <= 0 || carrier_freq <= 0) {
        return -EINVAL;
    }
    if (pattern_len > IRRC_MAX_PATTERN_ENTRIES) {
        ALOGE("EWG PCM envelope: pattern_len %d > max %d",
                pattern_len, IRRC_MAX_PATTERN_ENTRIES);
        return -EINVAL;
    }

    if (irrc_pattern_looks_pronto_cycles(carrier_freq, pattern, pattern_len)) {
        pattern_us = malloc(sizeof(int) * (size_t)pattern_len);
        if (pattern_us
                && irrc_convert_pronto_to_us(carrier_freq, pattern,
                        pattern_us, pattern_len) == 0) {
            xmit_pattern = pattern_us;
            converted = 1;
            ALOGI("Pronto cycles->us: header %d->%d (freq=%d)",
                    pattern[0], pattern_us[0], carrier_freq);
        } else {
            free(pattern_us);
            pattern_us = NULL;
            ALOGW("Pronto convert alloc/convert failed; using raw pattern");
        }
    }

    {
        char preview[128];
        int n = 0;
        int lim = pattern_len < 12 ? pattern_len : 12;
        int j;

        n += snprintf(preview + n, sizeof(preview) - n, "pattern:");
        for (j = 0; j < lim && n < (int)sizeof(preview) - 1; j++) {
            n += snprintf(preview + n, sizeof(preview) - n, " %d",
                    xmit_pattern[j]);
        }
        ALOGI("transmit %d entries at %d Hz%s; %s",
                pattern_len, carrier_freq,
                converted ? " (Pronto->us)" : "", preview);
    }

    pthread_mutex_lock(&g_lock);

    rc = irrc_transmit_ewg_pcm(carrier_freq, xmit_pattern, pattern_len);
    if (rc == -EAGAIN) {
        ALOGW("EWG PCM envelope unavailable; trying last-resort IRRC_TRANSMIT");
        rc = irrc_transmit_kernel_fallback(carrier_freq, xmit_pattern,
                pattern_len);
    }

    pthread_mutex_unlock(&g_lock);
    free(pattern_us);
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
