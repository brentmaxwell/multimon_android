/*
 * audio_pipe.c - Lock-free ring buffer implementation
 */

#include "audio_pipe.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* Audio pipe structure */
struct audio_pipe {
    int16_t* buffer;           /* Ring buffer */
    size_t size;               /* Buffer size (power of 2) */
    size_t mask;               /* Size mask for wrap-around */
    atomic_size_t read_pos;    /* Read position (consumer) */
    atomic_size_t write_pos;   /* Write position (producer) */
};

/* Round up to next power of 2 */
static size_t next_power_of_2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}

audio_pipe_t* audio_pipe_create(size_t size) {
    audio_pipe_t* pipe = (audio_pipe_t*)malloc(sizeof(audio_pipe_t));
    if (!pipe) return NULL;
    
    /* Round size to power of 2 for efficient modulo */
    pipe->size = next_power_of_2(size);
    pipe->mask = pipe->size - 1;
    
    /* Allocate buffer */
    pipe->buffer = (int16_t*)calloc(pipe->size, sizeof(int16_t));
    if (!pipe->buffer) {
        free(pipe);
        return NULL;
    }
    
    /* Initialize positions */
    atomic_init(&pipe->read_pos, 0);
    atomic_init(&pipe->write_pos, 0);
    
    return pipe;
}

void audio_pipe_destroy(audio_pipe_t* pipe) {
    if (!pipe) return;
    if (pipe->buffer) free(pipe->buffer);
    free(pipe);
}

size_t audio_pipe_write(audio_pipe_t* pipe, const int16_t* samples, size_t count) {
    if (!pipe || !samples || count == 0) return 0;
    
    size_t write_pos = atomic_load(&pipe->write_pos);
    size_t read_pos = atomic_load(&pipe->read_pos);
    
    /* Calculate free space (leave one sample empty to distinguish full/empty) */
    size_t free = (read_pos - write_pos - 1) & pipe->mask;
    if (count > free) count = free;
    
    /* Write in one or two chunks (wrap-around) */
    size_t chunk1 = pipe->size - (write_pos & pipe->mask);
    if (chunk1 > count) chunk1 = count;
    
    memcpy(&pipe->buffer[write_pos & pipe->mask], samples, chunk1 * sizeof(int16_t));
    
    if (count > chunk1) {
        size_t chunk2 = count - chunk1;
        memcpy(&pipe->buffer[0], &samples[chunk1], chunk2 * sizeof(int16_t));
    }
    
    /* Update write position */
    atomic_store(&pipe->write_pos, (write_pos + count) & pipe->mask);
    
    return count;
}

size_t audio_pipe_read(audio_pipe_t* pipe, int16_t* samples, size_t count) {
    if (!pipe || !samples || count == 0) return 0;
    
    size_t read_pos = atomic_load(&pipe->read_pos);
    size_t write_pos = atomic_load(&pipe->write_pos);
    
    /* Calculate available data */
    size_t available = (write_pos - read_pos) & pipe->mask;
    if (count > available) count = available;
    
    /* Read in one or two chunks (wrap-around) */
    size_t chunk1 = pipe->size - (read_pos & pipe->mask);
    if (chunk1 > count) chunk1 = count;
    
    memcpy(samples, &pipe->buffer[read_pos & pipe->mask], chunk1 * sizeof(int16_t));
    
    if (count > chunk1) {
        size_t chunk2 = count - chunk1;
        memcpy(&samples[chunk1], &pipe->buffer[0], chunk2 * sizeof(int16_t));
    }
    
    /* Update read position */
    atomic_store(&pipe->read_pos, (read_pos + count) & pipe->mask);
    
    return count;
}

size_t audio_pipe_available(audio_pipe_t* pipe) {
    if (!pipe) return 0;
    size_t read_pos = atomic_load(&pipe->read_pos);
    size_t write_pos = atomic_load(&pipe->write_pos);
    return (write_pos - read_pos) & pipe->mask;
}

size_t audio_pipe_free_space(audio_pipe_t* pipe) {
    if (!pipe) return 0;
    size_t read_pos = atomic_load(&pipe->read_pos);
    size_t write_pos = atomic_load(&pipe->write_pos);
    return (read_pos - write_pos - 1) & pipe->mask;
}

void audio_pipe_reset(audio_pipe_t* pipe) {
    if (!pipe) return;
    atomic_store(&pipe->read_pos, 0);
    atomic_store(&pipe->write_pos, 0);
}
