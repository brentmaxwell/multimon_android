/*
 * rtl_fm_core.h - Core FM demod functions extracted from rtl_fm
 * Minimal wrapper around rtl_fm's demod functions
 */

#ifndef RTL_FM_CORE_H
#define RTL_FM_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to demod state */
typedef struct rtl_fm_demod rtl_fm_demod_t;

/**
 * Create FM demodulator using rtl_fm's proven code
 * @param input_rate Input IQ sample rate
 * @param output_rate Desired audio output rate
 * @return Demodulator handle or NULL on error
 */
rtl_fm_demod_t* rtl_fm_demod_create(uint32_t input_rate, uint32_t output_rate);

/**
 * Process IQ buffer through FM demodulation
 * This applies rotate_90, converts to int16, low-pass filters, and FM demods
 * @param demod Demodulator handle
 * @param iq_buf Input IQ samples (uint8_t, interleaved I/Q)
 * @param iq_len Number of bytes in iq_buf
 * @param audio_out Output buffer for audio (int16_t)
 * @param audio_max Maximum samples to write to audio_out
 * @return Number of audio samples written
 */
size_t rtl_fm_demod_process(rtl_fm_demod_t* demod,
                             const uint8_t* iq_buf,
                             size_t iq_len,
                             int16_t* audio_out,
                             size_t audio_max);

/**
 * Destroy demodulator and free resources
 */
void rtl_fm_demod_destroy(rtl_fm_demod_t* demod);

#ifdef __cplusplus
}
#endif

#endif /* RTL_FM_CORE_H */
