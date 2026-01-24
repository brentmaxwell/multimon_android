/*
 * fm_demod.h - Simplified FM demodulator for SDR
 * Based on rtl_fm by Kyle Keen
 */

#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FM demodulator handle */
typedef struct fm_demod fm_demod_t;

/* FM demodulation mode */
typedef enum {
    FM_MODE_FM,      /* Standard FM */
    FM_MODE_AM,      /* AM */
    FM_MODE_USB,     /* USB */
    FM_MODE_LSB,     /* LSB */
    FM_MODE_RAW      /* Raw IQ */
} fm_mode_t;

/**
 * Create FM demodulator
 * @param input_rate Input sample rate (from RTL-SDR)
 * @param output_rate Desired output sample rate
 * @param mode Demodulation mode
 * @return Demodulator handle or NULL on error
 */
fm_demod_t* fm_demod_create(uint32_t input_rate, uint32_t output_rate, fm_mode_t mode);

/**
 * Process IQ samples and output audio
 * @param demod Demodulator handle
 * @param iq_samples Input IQ samples (unsigned 8-bit, interleaved I/Q)
 * @param iq_count Number of IQ samples (must be even)
 * @param audio_out Output audio buffer (int16_t)
 * @param audio_max Maximum number of audio samples to write
 * @return Number of audio samples written
 */
size_t fm_demod_process(fm_demod_t* demod,
                        const uint8_t* iq_samples,
                        size_t iq_count,
                        int16_t* audio_out,
                        size_t audio_max);

/**
 * Set squelch level
 * @param demod Demodulator handle
 * @param level Squelch level (0 = disabled, higher = more aggressive)
 */
void fm_demod_set_squelch(fm_demod_t* demod, int level);

/**
 * Set de-emphasis filter (for FM broadcast)
 * @param demod Demodulator handle
 * @param enable 1 to enable, 0 to disable
 */
void fm_demod_set_deemph(fm_demod_t* demod, int enable);

/**
 * Set rotate_90 mode for offset tuning compensation
 * @param demod Demodulator handle
 * @param enable 1 to enable (default), 0 to disable
 */
void fm_demod_set_rotate_90(fm_demod_t* demod, int enable);

/**
 * Reset demodulator state
 * @param demod Demodulator handle
 */
void fm_demod_reset(fm_demod_t* demod);

/**
 * Destroy FM demodulator and free resources
 * @param demod Demodulator handle
 */
void fm_demod_destroy(fm_demod_t* demod);

#ifdef __cplusplus
}
#endif

#endif /* FM_DEMOD_H */
