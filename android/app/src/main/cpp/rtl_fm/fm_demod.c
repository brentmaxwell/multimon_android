/*
 * fm_demod.c - FM demodulator for SDR
 * Based directly on rtl_fm by Kyle Keen and Steve Markgraf
 */

#include "fm_demod.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* FM demodulator state */
struct fm_demod {
    /* Configuration */
    uint32_t input_rate;      /* RTL-SDR sample rate */
    uint32_t output_rate;     /* Audio output rate */
    fm_mode_t mode;           /* Demodulation mode */
    int downsample;           /* Downsample factor */
    int squelch_level;        /* Squelch threshold */
    int use_deemph;           /* De-emphasis filter enabled */
    int use_rotate_90;        /* Apply rotate_90 for offset tuning */
    
    /* Processing buffers */
    int16_t *lowpassed;       /* After low-pass filter */
    int lp_len;               /* Length of lowpassed data */
    int16_t *result;          /* Demodulated audio */
    int result_len;           /* Length of result */
    
    /* Working buffer for rotation (needs to be mutable) */
    uint8_t *iq_work;         /* Working copy of IQ data */
    size_t iq_work_size;
    
    /* Filter state - use int32 to handle accumulation without overflow */
    int32_t now_r, now_j;     /* Current accumulator for low-pass */
    int prev_index;           /* Downsample counter */
    int pre_r, pre_j;         /* Previous sample for FM discriminant */
    
    /* DC blocking */
    int dc_avg;               /* Running DC average */
    
    /* De-emphasis filter state */
    int deemph_a;             /* De-emphasis alpha */
    int adc, bdc;             /* De-emphasis state */
    
    /* Squelch */
    int squelch_hits;         /* Counter for squelch */
};

/* Compute optimal downsample factor */
static int compute_downsample(uint32_t input_rate, uint32_t output_rate) {
    int ds = input_rate / output_rate;
    if (ds < 1) ds = 1;
    return ds;
}

fm_demod_t* fm_demod_create(uint32_t input_rate, uint32_t output_rate, fm_mode_t mode) {
    fm_demod_t* d = (fm_demod_t*)calloc(1, sizeof(fm_demod_t));
    if (!d) return NULL;
    
    d->input_rate = input_rate;
    d->output_rate = output_rate;
    d->mode = mode;
    d->downsample = compute_downsample(input_rate, output_rate);
    d->squelch_level = 0;
    d->use_deemph = 0;
    d->use_rotate_90 = 0;  /* Disabled by default for RTL-TCP (server handles tuning) */
    
    /* Allocate buffers - sized for maximum expected input */
    size_t max_samples = 262144;  /* 256K samples max */
    d->lowpassed = (int16_t*)calloc(max_samples, sizeof(int16_t));
    d->result = (int16_t*)calloc(max_samples, sizeof(int16_t));
    d->iq_work = (uint8_t*)malloc(max_samples);
    d->iq_work_size = max_samples;
    
    if (!d->lowpassed || !d->result || !d->iq_work) {
        fm_demod_destroy(d);
        return NULL;
    }
    
    /* Initialize de-emphasis for 75us (standard for NBFM) */
    d->deemph_a = (int)round((1.0 - exp(-1.0 / (output_rate * 75e-6))) * 32768.0);
    
    return d;
}

void fm_demod_destroy(fm_demod_t* demod) {
    if (!demod) return;
    if (demod->lowpassed) free(demod->lowpassed);
    if (demod->result) free(demod->result);
    if (demod->iq_work) free(demod->iq_work);
    free(demod);
}

void fm_demod_set_squelch(fm_demod_t* demod, int level) {
    if (demod) demod->squelch_level = level;
}

void fm_demod_set_deemph(fm_demod_t* demod, int enable) {
    if (demod) demod->use_deemph = enable;
}

void fm_demod_set_rotate_90(fm_demod_t* demod, int enable) {
    if (demod) demod->use_rotate_90 = enable;
}

void fm_demod_reset(fm_demod_t* demod) {
    if (!demod) return;
    demod->now_r = 0;
    demod->now_j = 0;
    demod->prev_index = 0;
    demod->pre_r = 0;
    demod->pre_j = 0;
    demod->dc_avg = 0;
    demod->adc = 0;
    demod->bdc = 0;
    demod->squelch_hits = 0;
}

/* 90 degree rotation for offset tuning compensation
   From rtl_fm: 90 rotation is 1+0j, 0+1j, -1+0j, 0-1j
   or [0, 1, -3, 2, -4, -5, 7, -6] */
static void rotate_90(uint8_t *buf, uint32_t len) {
    uint32_t i;
    uint8_t tmp;
    for (i = 0; i < len; i += 8) {
        /* uint8_t negation = 255 - x */
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

/* Complex multiply: (ar + aj*i) * (br + bj*i) */
static void multiply(int ar, int aj, int br, int bj, int *cr, int *cj) {
    *cr = ar*br - aj*bj;
    *cj = aj*br + ar*bj;
}

/* Polar discriminant - from rtl_fm */
static int polar_discriminant(int ar, int aj, int br, int bj) {
    int cr, cj;
    double angle;
    /* multiply by conjugate of b: (ar+aj*i) * (br-bj*i) */
    multiply(ar, aj, br, -bj, &cr, &cj);
    angle = atan2((double)cj, (double)cr);
    return (int)(angle / M_PI * (1<<14));
}

/* Fast atan2 - from rtl_fm */
static int fast_atan2(int y, int x) {
    int yabs, angle;
    int pi4=(1<<12), pi34=3*(1<<12);
    if (x==0 && y==0) {
        return 0;
    }
    yabs = y;
    if (yabs < 0) {
        yabs = -yabs;
    }
    if (x >= 0) {
        angle = pi4 - pi4 * (x-yabs) / (x+yabs);
    } else {
        angle = pi34 - pi4 * (x+yabs) / (yabs-x);
    }
    if (y < 0) {
        return -angle;
    }
    return angle;
}

/* Fast polar discriminant - from rtl_fm */
static int polar_disc_fast(int ar, int aj, int br, int bj) {
    int cr, cj;
    multiply(ar, aj, br, -bj, &cr, &cj);
    return fast_atan2(cj, cr);
}

/* Low-pass filter with downsampling - from rtl_fm
   NOTE: rtl_fm does NOT divide the accumulated values, keeping more precision */
static void low_pass(fm_demod_t* d) {
    int i = 0, i2 = 0;
    int16_t *lp = d->lowpassed;
    
    while (i < d->lp_len) {
        d->now_r += lp[i];
        d->now_j += lp[i+1];
        i += 2;
        d->prev_index++;
        if (d->prev_index < d->downsample) {
            continue;
        }
        /* rtl_fm stores accumulated value, not divided
           This gives larger values but fm_demod handles the range */
        lp[i2]   = (int16_t)d->now_r;
        lp[i2+1] = (int16_t)d->now_j;
        d->prev_index = 0;
        d->now_r = 0;
        d->now_j = 0;
        i2 += 2;
    }
    d->lp_len = i2;
}

/* FM demodulation - simplified based on dsd-neo approach */
static void fm_demod_fm(fm_demod_t* d) {
    if (d->lp_len < 4) {  /* Need at least 2 IQ pairs */
        d->result_len = 0;
        return;
    }
    
    int pairs = d->lp_len / 2;
    int16_t *lp = d->lowpassed;
    
    /* Use float for better precision in phase calculations */
    float prev_r = (float)d->pre_r;
    float prev_j = (float)d->pre_j;
    
    /* Seed history on first use */
    if (prev_r == 0.0f && prev_j == 0.0f) {
        prev_r = (float)lp[0];
        prev_j = (float)lp[1];
    }
    
    for (int n = 0; n < pairs; n++) {
        float cr = (float)lp[n * 2 + 0];
        float cj = (float)lp[n * 2 + 1];
        
        /* Phase discriminator: z_n * conj(z_{n-1}) 
           Complex conjugate: conj(a + bi) = a - bi
           Multiply: (cr + cj*i) * (prev_r - prev_j*i)
                   = (cr*prev_r + cj*prev_j) + (cj*prev_r - cr*prev_j)*i */
        float re = cr * prev_r + cj * prev_j;
        float im = cj * prev_r - cr * prev_j;
        
        /* Angle is the FM demodulated output */
        float angle = atan2f(im, re);
        
        /* Scale to int16 range: angle is in [-π, π], scale to ±32767
           angle/π gives [-1, 1], multiply by 32767 */
        int32_t sample = (int32_t)(angle * (32767.0f / M_PI));
        
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        d->result[n] = (int16_t)sample;
        
        prev_r = cr;
        prev_j = cj;
    }
    
    d->pre_r = (int)prev_r;
    d->pre_j = (int)prev_j;
    d->result_len = pairs;
}

/* DC blocking filter */
static void dc_block(fm_demod_t* d) {
    int i;
    int64_t sum = 0;
    
    if (d->result_len == 0) return;
    
    for (i = 0; i < d->result_len; i++) {
        sum += d->result[i];
    }
    int avg = (int)(sum / d->result_len);
    
    /* Slow-moving average */
    avg = (avg + d->dc_avg * 9) / 10;
    
    for (i = 0; i < d->result_len; i++) {
        d->result[i] -= avg;
    }
    d->dc_avg = avg;
}

size_t fm_demod_process(fm_demod_t* demod,
                        const uint8_t* iq_samples,
                        size_t iq_count,
                        int16_t* audio_out,
                        size_t audio_max) {
    size_t i;
    uint8_t* work_buf;
    
    if (!demod || !iq_samples || iq_count == 0) return 0;
    if (!audio_out || audio_max == 0) return 0;
    
    /* Ensure even count (IQ pairs) and multiple of 8 for rotate_90 */
    iq_count &= ~7;  /* Round down to multiple of 8 */
    if (iq_count == 0) return 0;
    
    /* Check if we need to resize work buffer */
    if (iq_count > demod->iq_work_size) {
        uint8_t* new_buf = (uint8_t*)realloc(demod->iq_work, iq_count);
        if (!new_buf) return 0;
        demod->iq_work = new_buf;
        demod->iq_work_size = iq_count;
    }
    
    /* Copy to working buffer for rotation */
    memcpy(demod->iq_work, iq_samples, iq_count);
    work_buf = demod->iq_work;
    
    /* Apply rotate_90 if enabled (compensates for offset tuning) */
    if (demod->use_rotate_90) {
        rotate_90(work_buf, (uint32_t)iq_count);
    }
    
    /* Convert to signed int16 - note: use 127 not 128 per rtl_fm */
    for (i = 0; i < iq_count; i++) {
        demod->lowpassed[i] = (int16_t)work_buf[i] - 127;
    }
    demod->lp_len = (int)iq_count;
    
    /* Low-pass filter and downsample */
    low_pass(demod);
    
    if (demod->lp_len < 4) return 0;  /* Need at least 2 IQ samples */
    
    /* FM demodulation (now outputs directly to ±32767 range) */
    fm_demod_fm(demod);
    
    /* Skip DC blocking for now - AFSK doesn't need it and it may attenuate signal */
    /* dc_block(demod); */
    
    /* Copy directly to output - no additional scaling needed */
    for (i = 0; i < (size_t)demod->result_len && i < audio_max; i++) {
        audio_out[i] = demod->result[i];
    }
    
    size_t out_count = (size_t)demod->result_len;
    if (out_count > audio_max) out_count = audio_max;
    
    return out_count;
}
