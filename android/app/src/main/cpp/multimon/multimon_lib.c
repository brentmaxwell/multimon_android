/*
 * multimon_lib.c - Library wrapper for multimon-ng
 */

#include "multimon_lib.h"
#include "multimon.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* Maximum decoders we can enable */
#define MAX_DECODERS 16

/* Maximum audio buffer size */
#define MAX_AUDIO_BUFFER 65536

/* Global variables required by multimon-ng */
int json_mode = 0;
int timestamp_mode = 0;
extern int aprs_mode;  /* Defined in hdlc.c */

/* Context structure */
struct multimon_ctx {
    /* Callback */
    multimon_callback_t callback;
    void* user_data;
    
    /* Audio settings */
    int sample_rate;
    
    /* Enabled decoders */
    struct demod_state* demod_states[MAX_DECODERS];
    const struct demod_param* demod_params[MAX_DECODERS];
    int num_decoders;
    
    /* Processing buffer with overlap */
    float* audio_buffer;
    int audio_buffer_size;
    int overlap;
    int buffer_pos;  /* Current position in buffer */
    
    /* Line buffering for output */
    char line_buffer[4096];
    int line_pos;
};

/* Global context pointer for verbprintf callback */
static multimon_ctx_t* g_ctx = NULL;

/* Stub functions required by multimon-ng */
void addJsonTimestamp(cJSON *json) {
    /* Stub - not using JSON mode */
    (void)json;
}

int xdisp_start(void) {
    return 0;
}

int xdisp_update(int cnum, float *f) {
    (void)cnum;
    (void)f;
    return 0;
}

/* verbprintf implementation - this is where decoded messages come from */
void _verbprintf(int verb_level, const char *fmt, ...) {
    char buffer[4096];
    va_list args;
    
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    /* Only pass level 0 messages (decoded data) to callback */
    if (verb_level == 0 && g_ctx && g_ctx->callback) {
        /* Buffer output character by character until we hit newline */
        int len = strlen(buffer);
        for (int i = 0; i < len; i++) {
            char c = buffer[i];
            
            if (c == '\n') {
                /* End of line - send complete message */
                if (g_ctx->line_pos > 0) {
                    g_ctx->line_buffer[g_ctx->line_pos] = '\0';
                    
                    /* Try to determine which decoder produced this */
                    const char* decoder_name = "UNKNOWN";
                    
                    /* Check for known prefixes */
                    if (strncmp(g_ctx->line_buffer, "APRS:", 5) == 0 || 
                        strncmp(g_ctx->line_buffer, "AFSK1200:", 9) == 0) {
                        decoder_name = "AFSK1200";
                    } else if (strncmp(g_ctx->line_buffer, "POCSAG", 6) == 0) {
                        decoder_name = "POCSAG";
                    } else if (strncmp(g_ctx->line_buffer, "FLEX", 4) == 0) {
                        decoder_name = "FLEX";
                    } else if (strncmp(g_ctx->line_buffer, "EAS", 3) == 0) {
                        decoder_name = "EAS";
                    } else if (strncmp(g_ctx->line_buffer, "DTMF", 4) == 0) {
                        decoder_name = "DTMF";
                    } else if (strncmp(g_ctx->line_buffer, "ZVEI", 4) == 0) {
                        decoder_name = "ZVEI";
                    } else if (strncmp(g_ctx->line_buffer, "MORSE", 5) == 0) {
                        decoder_name = "MORSE";
                    }
                    
                    g_ctx->callback(decoder_name, g_ctx->line_buffer, g_ctx->user_data);
                    g_ctx->line_pos = 0;
                }
            } else if (g_ctx->line_pos < sizeof(g_ctx->line_buffer) - 1) {
                /* Add character to buffer */
                g_ctx->line_buffer[g_ctx->line_pos++] = c;
            }
        }
    }
}

/* Map decoder enum to demod_param */
static const struct demod_param* get_demod_param(multimon_decoder_t decoder) {
    switch (decoder) {
        case MULTIMON_AFSK1200:    return &demod_afsk1200;
        case MULTIMON_AFSK2400:    return &demod_afsk2400;
        case MULTIMON_AFSK2400_2:  return &demod_afsk2400_2;
        case MULTIMON_AFSK2400_3:  return &demod_afsk2400_3;
        case MULTIMON_POCSAG512:   return &demod_poc5;
        case MULTIMON_POCSAG1200:  return &demod_poc12;
        case MULTIMON_POCSAG2400:  return &demod_poc24;
        case MULTIMON_EAS:         return &demod_eas;
        case MULTIMON_UFSK1200:    return &demod_ufsk1200;
        case MULTIMON_CLIPFSK:     return &demod_clipfsk;
        case MULTIMON_FMSFSK:      return &demod_fmsfsk;
        case MULTIMON_DTMF:        return &demod_dtmf;
        case MULTIMON_ZVEI1:       return &demod_zvei1;
        case MULTIMON_ZVEI2:       return &demod_zvei2;
        case MULTIMON_ZVEI3:       return &demod_zvei3;
        case MULTIMON_DZVEI:       return &demod_dzvei;
        case MULTIMON_PZVEI:       return &demod_pzvei;
        case MULTIMON_EEA:         return &demod_eea;
        case MULTIMON_EIA:         return &demod_eia;
        case MULTIMON_CCIR:        return &demod_ccir;
        case MULTIMON_FLEX:        return &demod_flex;
        case MULTIMON_HAPN4800:    return &demod_hapn4800;
        case MULTIMON_FSK9600:     return &demod_fsk9600;
        case MULTIMON_MORSE:       return &demod_morse;
        case MULTIMON_X10:         return &demod_x10;
        default:                   return NULL;
    }
}

multimon_ctx_t* multimon_create(multimon_callback_t callback, void* user_data, int sample_rate) {
    multimon_ctx_t* ctx = (multimon_ctx_t*)calloc(1, sizeof(multimon_ctx_t));
    if (!ctx) return NULL;
    
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->sample_rate = sample_rate;
    ctx->num_decoders = 0;
    ctx->overlap = 0;
    ctx->buffer_pos = 0;
    ctx->line_pos = 0;  /* Initialize line buffer position */
    
    /* Allocate audio buffer */
    ctx->audio_buffer_size = MAX_AUDIO_BUFFER;
    ctx->audio_buffer = (float*)calloc(ctx->audio_buffer_size, sizeof(float));
    if (!ctx->audio_buffer) {
        free(ctx);
        return NULL;
    }
    
    /* Set global context for verbprintf */
    g_ctx = ctx;
    
    return ctx;
}

int multimon_enable_decoder(multimon_ctx_t* ctx, multimon_decoder_t decoder) {
    if (!ctx || ctx->num_decoders >= MAX_DECODERS) return -1;
    
    const struct demod_param* param = get_demod_param(decoder);
    if (!param) return -1;
    
    /* Check sample rate compatibility */
    if (param->samplerate != (unsigned int)ctx->sample_rate) {
        /* Sample rate mismatch - could resample, but for now just warn */
        /* Most decoders want 22050 Hz */
    }
    
    /* Allocate demodulator state */
    struct demod_state* state = (struct demod_state*)calloc(1, sizeof(struct demod_state));
    if (!state) return -1;
    
    state->dem_par = param;
    
    /* Initialize decoder */
    if (param->init) {
        param->init(state);
    }
    
    /* Store decoder */
    ctx->demod_states[ctx->num_decoders] = state;
    ctx->demod_params[ctx->num_decoders] = param;
    ctx->num_decoders++;
    
    /* Update overlap requirement */
    if (param->overlap > (unsigned int)ctx->overlap) {
        ctx->overlap = param->overlap;
    }
    
    return 0;
}

void multimon_set_aprs_mode(multimon_ctx_t* ctx, int enable) {
    (void)ctx;
    aprs_mode = enable;
}

int multimon_get_overlap(multimon_ctx_t* ctx) {
    return ctx ? ctx->overlap : 0;
}

int multimon_process(multimon_ctx_t* ctx, const int16_t* samples, size_t count) {
    static int call_count = 0;
    if (!ctx || !samples || count == 0) return -1;
    
    /* Convert int16 to float and add to buffer */
    size_t space = ctx->audio_buffer_size - ctx->buffer_pos;
    if (count > space) count = space;
    
    for (size_t i = 0; i < count; i++) {
        ctx->audio_buffer[ctx->buffer_pos + i] = samples[i] / 32768.0f;
    }
    ctx->buffer_pos += count;
    
    /* Process if we have enough samples */
    int process_len = ctx->buffer_pos - ctx->overlap;
    if (process_len <= 0) return 0;
    
    call_count++;
    
    /* Debug first few calls */
    if (call_count <= 5) {
        float min_f = 1.0f, max_f = -1.0f;
        for (int i = 0; i < process_len && i < 100; i++) {
            if (ctx->audio_buffer[i] < min_f) min_f = ctx->audio_buffer[i];
            if (ctx->audio_buffer[i] > max_f) max_f = ctx->audio_buffer[i];
        }
        verbprintf(1, "[Multimon] Processing %d samples, float range [%.3f, %.3f]\n", 
                   process_len, min_f, max_f);
    }
    
    /* Create buffer struct for demodulators */
    buffer_t buffer;
    buffer.fbuffer = ctx->audio_buffer;
    buffer.sbuffer = NULL;
    
    /* Process through all enabled decoders */
    for (int i = 0; i < ctx->num_decoders; i++) {
        if (ctx->demod_states[i] && ctx->demod_params[i] && ctx->demod_params[i]->demod) {
            ctx->demod_params[i]->demod(ctx->demod_states[i], buffer, process_len);
        }
    }
    
    /* Move overlap to beginning */
    if (ctx->overlap > 0 && ctx->buffer_pos > ctx->overlap) {
        memmove(ctx->audio_buffer, 
                ctx->audio_buffer + process_len, 
                ctx->overlap * sizeof(float));
        ctx->buffer_pos = ctx->overlap;
    } else {
        ctx->buffer_pos = 0;
    }
    
    return 0;
}

int multimon_process_float(multimon_ctx_t* ctx, const float* samples, size_t count) {
    if (!ctx || !samples || count == 0) return -1;
    
    /* Add to buffer */
    size_t space = ctx->audio_buffer_size - ctx->buffer_pos;
    if (count > space) count = space;
    
    memcpy(ctx->audio_buffer + ctx->buffer_pos, samples, count * sizeof(float));
    ctx->buffer_pos += count;
    
    /* Process if we have enough samples */
    int process_len = ctx->buffer_pos - ctx->overlap;
    if (process_len <= 0) return 0;
    
    /* Create buffer struct for demodulators */
    buffer_t buffer;
    buffer.fbuffer = ctx->audio_buffer;
    buffer.sbuffer = NULL;
    
    /* Process through all enabled decoders */
    for (int i = 0; i < ctx->num_decoders; i++) {
        if (ctx->demod_states[i] && ctx->demod_params[i] && ctx->demod_params[i]->demod) {
            ctx->demod_params[i]->demod(ctx->demod_states[i], buffer, process_len);
        }
    }
    
    /* Move overlap to beginning */
    if (ctx->overlap > 0 && ctx->buffer_pos > ctx->overlap) {
        memmove(ctx->audio_buffer, 
                ctx->audio_buffer + process_len, 
                ctx->overlap * sizeof(float));
        ctx->buffer_pos = ctx->overlap;
    } else {
        ctx->buffer_pos = 0;
    }
    
    return 0;
}

void multimon_destroy(multimon_ctx_t* ctx) {
    if (!ctx) return;
    
    /* Deinitialize decoders */
    for (int i = 0; i < ctx->num_decoders; i++) {
        if (ctx->demod_states[i]) {
            if (ctx->demod_params[i] && ctx->demod_params[i]->deinit) {
                ctx->demod_params[i]->deinit(ctx->demod_states[i]);
            }
            free(ctx->demod_states[i]);
        }
    }
    
    /* Free buffer */
    if (ctx->audio_buffer) free(ctx->audio_buffer);
    
    /* Clear global context */
    if (g_ctx == ctx) g_ctx = NULL;
    
    free(ctx);
}
