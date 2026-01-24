/*
 * audio_pipe.h - Lock-free ring buffer for audio data between rtl_fm and multimon-ng
 * 
 * This provides a thread-safe circular buffer where:
 * - rtl_fm (producer) writes PCM audio samples
 * - multimon-ng (consumer) reads PCM audio samples
 */

#ifndef AUDIO_PIPE_H
#define AUDIO_PIPE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio pipe handle */
typedef struct audio_pipe audio_pipe_t;

/**
 * Create a new audio pipe
 * @param size Buffer size in samples (will be rounded up to power of 2)
 * @return Pipe handle or NULL on error
 */
audio_pipe_t* audio_pipe_create(size_t size);

/**
 * Destroy audio pipe
 * @param pipe Pipe handle
 */
void audio_pipe_destroy(audio_pipe_t* pipe);

/**
 * Write samples to pipe (producer - rtl_fm)
 * @param pipe Pipe handle
 * @param samples Array of int16_t samples
 * @param count Number of samples to write
 * @return Number of samples actually written (may be less if buffer full)
 */
size_t audio_pipe_write(audio_pipe_t* pipe, const int16_t* samples, size_t count);

/**
 * Read samples from pipe (consumer - multimon-ng)
 * @param pipe Pipe handle
 * @param samples Output buffer for int16_t samples
 * @param count Maximum number of samples to read
 * @return Number of samples actually read (may be less if buffer empty)
 */
size_t audio_pipe_read(audio_pipe_t* pipe, int16_t* samples, size_t count);

/**
 * Get number of samples available to read
 * @param pipe Pipe handle
 * @return Number of samples available
 */
size_t audio_pipe_available(audio_pipe_t* pipe);

/**
 * Get free space in pipe (for writing)
 * @param pipe Pipe handle
 * @return Number of samples that can be written
 */
size_t audio_pipe_free_space(audio_pipe_t* pipe);

/**
 * Reset pipe (clear all data)
 * @param pipe Pipe handle
 */
void audio_pipe_reset(audio_pipe_t* pipe);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PIPE_H */
