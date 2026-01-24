/*
 *      rtl_input.c -- RTL-SDR input support for multimon-ng
 *
 *      Copyright (C) 2026
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 */

#include "multimon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <rtl-sdr.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_SAMPLE_RATE     22050
#define DEFAULT_BUF_LENGTH      (1 * 16384)
#define MAXIMUM_OVERSAMPLE      48
#define MAXIMUM_BUF_LENGTH      (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define AUTO_GAIN              -100

struct rtl_dongle_state {
    pthread_t thread;
    rtlsdr_dev_t *dev;
    int dev_index;
    uint32_t freq;
    uint32_t rate;
    int gain;
    int16_t buf16[MAXIMUM_BUF_LENGTH];
    uint32_t buf_len;
    int ppm_error;
    int offset_tuning;
    int direct_sampling;
    volatile int exit_flag;
};

struct rtl_demod_state {
    pthread_t thread;
    int16_t lowpassed[MAXIMUM_BUF_LENGTH];
    int lp_len;
    int16_t result[MAXIMUM_BUF_LENGTH];
    int result_len;
    int rate_in;
    int rate_out;
    int downsample;
    int prev_index;
    int now_r, now_j;
    int pre_r, pre_j;
    int dc_avg;
    pthread_rwlock_t rw;
    pthread_cond_t ready;
    pthread_mutex_t ready_m;
    volatile int exit_flag;
};

struct rtl_output_state {
    pthread_t thread;
    int16_t *result;
    int result_len;
    pthread_rwlock_t rw;
    pthread_cond_t ready;
    pthread_mutex_t ready_m;
    volatile int exit_flag;
    void (*process_callback)(int16_t *buffer, int length);
};

static struct rtl_dongle_state dongle;
static struct rtl_demod_state demod_rtl;
static struct rtl_output_state output_rtl;
static volatile int do_exit_rtl = 0;

#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

void rotate_90(unsigned char *buf, uint32_t len)
{
    uint32_t i;
    unsigned char tmp;
    for (i=0; i<len; i+=8) {
        tmp = 255 - buf[i+3];
        buf[i+3] = buf[i+2];
        buf[i+2] = tmp;

        buf[i+4] = 255 - buf[i+4];
        buf[i+5] = 255 - buf[i+5];

        tmp = 255 - buf[i+6];
        buf[i+6] = buf[i+7];
        buf[i+7] = tmp;
    }
}

int polar_discriminant(int ar, int aj, int br, int bj)
{
    int cr, cj;
    double angle;
    /* multiply by conjugate of b: (ar + aj*i) * (br - bj*i) */
    cr = ar*br + aj*bj;
    cj = aj*br - ar*bj;
    angle = atan2((double)cj, (double)cr);
    return (int)(angle / M_PI * (1<<14));
}

void low_pass(struct rtl_demod_state *d)
{
    int i=0, i2=0;
    
    while (i < d->lp_len) {
        d->now_r += d->lowpassed[i];
        d->now_j += d->lowpassed[i+1];
        i += 2;
        d->prev_index++;
        if (d->prev_index < d->downsample) {
            continue;
        }
        d->lowpassed[i2]   = d->now_r;
        d->lowpassed[i2+1] = d->now_j;
        d->prev_index = 0;
        d->now_r = 0;
        d->now_j = 0;
        i2 += 2;
    }
    d->lp_len = i2;
}

void fm_demod(struct rtl_demod_state *d)
{
    int i, pcm;
    int16_t *lp = d->lowpassed;
    
    pcm = polar_discriminant(lp[0], lp[1], d->pre_r, d->pre_j);
    d->result[0] = (int16_t)pcm;
    
    for (i = 2; i < (d->lp_len-1); i += 2) {
        pcm = polar_discriminant(lp[i], lp[i+1], lp[i-2], lp[i-1]);
        d->result[i/2] = (int16_t)pcm;
    }
    
    d->pre_r = lp[d->lp_len - 2];
    d->pre_j = lp[d->lp_len - 1];
    d->result_len = d->lp_len / 2;
}

void dc_block_filter(struct rtl_demod_state *d)
{
    int i, avg;
    int64_t sum = 0;
    for (i=0; i < d->result_len; i++) {
        sum += d->result[i];
    }
    avg = sum / d->result_len;
    avg = (avg + d->dc_avg * 9) / 10;
    for (i=0; i < d->result_len; i++) {
        d->result[i] -= avg;
    }
    d->dc_avg = avg;
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
    int i;
    struct rtl_dongle_state *s = ctx;

    if (do_exit_rtl || !ctx) {
        return;
    }

    if (!s->offset_tuning) {
        rotate_90(buf, len);
    }

    for (i=0; i<(int)len; i++) {
        s->buf16[i] = (int16_t)buf[i] - 127;
    }

    pthread_rwlock_wrlock(&demod_rtl.rw);
    memcpy(demod_rtl.lowpassed, s->buf16, 2*len);
    demod_rtl.lp_len = len;
    pthread_rwlock_unlock(&demod_rtl.rw);
    safe_cond_signal(&demod_rtl.ready, &demod_rtl.ready_m);
}

static void *dongle_thread_fn(void *arg)
{
    struct rtl_dongle_state *s = arg;
    rtlsdr_read_async(s->dev, rtlsdr_callback, s, 0, s->buf_len);
    return 0;
}

static void *demod_thread_fn(void *arg)
{
    struct rtl_demod_state *d = arg;
    
    while (!do_exit_rtl) {
        safe_cond_wait(&d->ready, &d->ready_m);
        
        pthread_rwlock_wrlock(&d->rw);
        low_pass(d);
        fm_demod(d);
        dc_block_filter(d);
        pthread_rwlock_unlock(&d->rw);

        if (d->exit_flag) {
            do_exit_rtl = 1;
            break;
        }

        pthread_rwlock_wrlock(&output_rtl.rw);
        memcpy(output_rtl.result, d->result, 2*d->result_len);
        output_rtl.result_len = d->result_len;
        pthread_rwlock_unlock(&output_rtl.rw);
        safe_cond_signal(&output_rtl.ready, &output_rtl.ready_m);
    }
    return 0;
}

static void *output_thread_fn(void *arg)
{
    struct rtl_output_state *s = arg;
    
    while (!do_exit_rtl) {
        safe_cond_wait(&s->ready, &s->ready_m);
        
        pthread_rwlock_rdlock(&s->rw);
        if (s->process_callback && s->result_len > 0) {
            s->process_callback(s->result, s->result_len);
        }
        pthread_rwlock_unlock(&s->rw);

        if (s->exit_flag) {
            do_exit_rtl = 1;
            break;
        }
    }
    return 0;
}

int rtl_init(const char *device, uint32_t frequency, uint32_t sample_rate, int gain, int ppm_error, 
             void (*process_callback)(int16_t *buffer, int length))
{
    int r;
    int dev_index = 0;
    uint32_t capture_rate;
    int downsample;

    memset(&dongle, 0, sizeof(dongle));
    memset(&demod_rtl, 0, sizeof(demod_rtl));
    memset(&output_rtl, 0, sizeof(output_rtl));

    if (device != NULL && strlen(device) > 0) {
        dev_index = atoi(device);
    }

    /* Calculate optimal capture rate like rtl_fm does */
    /* downsample = (1000000 / rate_in) + 1 */
    downsample = (1000000 / sample_rate) + 1;
    capture_rate = downsample * sample_rate;
    
    /* Ensure capture rate is valid for RTL-SDR (225001-3200000 Hz) */
    if (capture_rate < 225001) {
        downsample = (225001 / sample_rate) + 1;
        capture_rate = downsample * sample_rate;
    }

    dongle.dev_index = dev_index;
    dongle.freq = frequency + capture_rate/4;  /* offset tuning */
    dongle.rate = capture_rate;
    dongle.gain = gain;
    dongle.ppm_error = ppm_error;
    dongle.buf_len = DEFAULT_BUF_LENGTH;
    dongle.offset_tuning = 0;
    dongle.direct_sampling = 0;

    demod_rtl.rate_in = capture_rate;
    demod_rtl.rate_out = sample_rate;
    demod_rtl.downsample = downsample;
    demod_rtl.prev_index = 0;
    demod_rtl.now_r = 0;
    demod_rtl.now_j = 0;
    demod_rtl.pre_r = 0;
    demod_rtl.pre_j = 0;
    demod_rtl.dc_avg = 0;

    output_rtl.process_callback = process_callback;
    output_rtl.result = malloc(MAXIMUM_BUF_LENGTH * sizeof(int16_t));
    if (!output_rtl.result) {
        fprintf(stderr, "Failed to allocate output buffer\n");
        return -1;
    }

    pthread_rwlock_init(&demod_rtl.rw, NULL);
    pthread_cond_init(&demod_rtl.ready, NULL);
    pthread_mutex_init(&demod_rtl.ready_m, NULL);

    pthread_rwlock_init(&output_rtl.rw, NULL);
    pthread_cond_init(&output_rtl.ready, NULL);
    pthread_mutex_init(&output_rtl.ready_m, NULL);

    r = rtlsdr_open(&dongle.dev, dongle.dev_index);
    if (r < 0) {
        fprintf(stderr, "Failed to open RTL-SDR device #%d\n", dongle.dev_index);
        free(output_rtl.result);
        return -1;
    }

    rtlsdr_set_sample_rate(dongle.dev, dongle.rate);
    rtlsdr_set_center_freq(dongle.dev, dongle.freq);
    
    if (dongle.gain == AUTO_GAIN) {
        rtlsdr_set_tuner_gain_mode(dongle.dev, 0);
    } else {
        rtlsdr_set_tuner_gain_mode(dongle.dev, 1);
        rtlsdr_set_tuner_gain(dongle.dev, dongle.gain);
    }

    if (dongle.ppm_error != 0) {
        rtlsdr_set_freq_correction(dongle.dev, dongle.ppm_error);
    }

    rtlsdr_reset_buffer(dongle.dev);

    fprintf(stderr, "RTL-SDR: Tuned to %u Hz (offset to %u Hz)\n", frequency, dongle.freq);
    fprintf(stderr, "RTL-SDR: Capture rate %u Hz, output %u Hz (downsample %dx)\n", 
            dongle.rate, sample_rate, downsample);

    return 0;
}

int rtl_start(void)
{
    pthread_create(&demod_rtl.thread, NULL, demod_thread_fn, &demod_rtl);
    pthread_create(&output_rtl.thread, NULL, output_thread_fn, &output_rtl);
    pthread_create(&dongle.thread, NULL, dongle_thread_fn, &dongle);
    return 0;
}

void rtl_stop(void)
{
    do_exit_rtl = 1;
    
    if (dongle.dev) {
        rtlsdr_cancel_async(dongle.dev);
    }

    safe_cond_signal(&demod_rtl.ready, &demod_rtl.ready_m);
    safe_cond_signal(&output_rtl.ready, &output_rtl.ready_m);

    if (dongle.thread) {
        pthread_join(dongle.thread, NULL);
    }
    if (demod_rtl.thread) {
        pthread_join(demod_rtl.thread, NULL);
    }
    if (output_rtl.thread) {
        pthread_join(output_rtl.thread, NULL);
    }

    if (dongle.dev) {
        rtlsdr_close(dongle.dev);
    }

    pthread_rwlock_destroy(&demod_rtl.rw);
    pthread_cond_destroy(&demod_rtl.ready);
    pthread_mutex_destroy(&demod_rtl.ready_m);

    pthread_rwlock_destroy(&output_rtl.rw);
    pthread_cond_destroy(&output_rtl.ready);
    pthread_mutex_destroy(&output_rtl.ready_m);

    if (output_rtl.result) {
        free(output_rtl.result);
    }
}
