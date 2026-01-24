/*
 * rtl_fm_lib.h - Library wrapper for rtl_fm
 * Provides a simple API to run rtl_fm as a library
 */

#ifndef RTL_FM_LIB_H
#define RTL_FM_LIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTL-FM handle */
typedef struct rtl_fm_context rtl_fm_context_t;

/* Audio callback - called when audio samples are ready */
typedef void (*rtl_fm_audio_callback_t)(int16_t* samples, size_t count, void* user_data);

/**
 * Create RTL-FM context
 * @param callback Audio output callback function
 * @param user_data User data passed to callback
 * @return Context handle or NULL on error
 */
rtl_fm_context_t* rtl_fm_create(rtl_fm_audio_callback_t callback, void* user_data);

/**
 * Configure RTL-FM (must be called before start)
 * @param ctx Context handle
 * @param frequency Center frequency in Hz
 * @param sample_rate Output sample rate in Hz (typically 22050)
 * @param gain Tuner gain in tenths of dB (e.g., 400 = 40.0 dB), or -100 for auto
 * @param ppm_error Frequency correction in PPM
 * @param squelch Squelch level (0 = off)
 * @return 0 on success, -1 on error
 */
int rtl_fm_configure(rtl_fm_context_t* ctx, 
                     uint32_t frequency,
                     uint32_t sample_rate,
                     int gain,
                     int ppm_error,
                     int squelch);

/**
 * Configure for RTL-TCP mode
 * @param ctx Context handle
 * @param host RTL-TCP server hostname/IP
 * @param port RTL-TCP server port
 * @return 0 on success, -1 on error
 */
int rtl_fm_configure_tcp(rtl_fm_context_t* ctx, const char* host, int port);

/**
 * Start RTL-FM (begins processing in background thread)
 * @param ctx Context handle
 * @param use_tcp If true, use RTL-TCP mode instead of USB
 * @return 0 on success, -1 on error
 */
int rtl_fm_start(rtl_fm_context_t* ctx, int use_tcp);

/**
 * Stop RTL-FM
 * @param ctx Context handle
 */
void rtl_fm_stop(rtl_fm_context_t* ctx);

/**
 * Destroy RTL-FM context and free resources
 * @param ctx Context handle
 */
void rtl_fm_destroy(rtl_fm_context_t* ctx);

/**
 * Check if RTL-FM is running
 * @param ctx Context handle
 * @return 1 if running, 0 if stopped
 */
int rtl_fm_is_running(rtl_fm_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* RTL_FM_LIB_H */
