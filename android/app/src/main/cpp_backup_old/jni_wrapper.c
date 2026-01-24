#include <jni.h>
#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <math.h>
#include "rtlsdr/librtlsdr/include/rtl-sdr.h"

#define LOG_TAG "MultimonRTLSDR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Include multimon header after defining verbprintf stub
#define MAX_VERBOSE_LEVEL 3
#include "multimon/multimon.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

// Global variables required by multimon-ng
int json_mode = 0;  // Disable JSON output mode
int timestamp_mode = 0;  // Disable timestamps
extern int aprs_mode;  // Defined in hdlc.c

static rtlsdr_dev_t *dev = NULL;
static int running = 0;
static pthread_t read_thread;
static struct demod_state *demod_states[16];
static const struct demod_param *demod_params[16];
static int num_demods = 0;
static JavaVM *g_jvm = NULL;
static jobject g_callback = NULL;

// RTL-TCP client state
static int tcp_socket = -1;
static int tcp_mode = 0;  // 0 = USB, 1 = TCP
static uint32_t tcp_sample_rate = 220500;  // 22050 * 10 for exact division
static uint32_t tcp_frequency = 144390000;

// Stub functions for multimon features we don't use
void addJsonTimestamp(cJSON *json) {
    // Stub - not using JSON mode
}

int xdisp_start(void) {
    // Stub - no X11 display on Android
    return 0;
}

int xdisp_update(int cnum, float *f) {
    // Stub - no X11 display on Android
    return 0;
}

// Implement _verbprintf (the real function that verbprintf macro calls)
void _verbprintf(int verb_level, const char *fmt, ...) {
    static int call_count = 0;
    call_count++;
    
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // Always log to Android logcat with level indicator
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[verbprintf level=%d #%d] %s", 
                       verb_level, call_count, buffer);
    
    // Send only level 0 to Flutter (decoded packets only)
    // Debug output (level 1+) stays in logcat only
    if (verb_level == 0) {
        if (g_jvm && g_callback) {
            JNIEnv *env;
            int attached = 0;
            
            if ((*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
                    LOGE("Failed to attach thread for callback");
                    return;
                }
                attached = 1;
            }
            
            jclass cls = (*env)->GetObjectClass(env, g_callback);
            jmethodID mid = (*env)->GetMethodID(env, cls, "onDataDecoded", "(Ljava/lang/String;)V");
            
            if (mid) {
                jstring jdata = (*env)->NewStringUTF(env, buffer);
                (*env)->CallVoidMethod(env, g_callback, mid, jdata);
                (*env)->DeleteLocalRef(env, jdata);
            } else {
                LOGE("Method onDataDecoded not found!");
            }
            
            (*env)->DeleteLocalRef(env, cls);
            
            if (attached) {
                (*g_jvm)->DetachCurrentThread(g_jvm);
            }
        }
    }
}

// Callback for decoded data (kept for compatibility, but verbprintf now handles it)
void multimon_output_callback(const char *data) {
    if (!g_jvm || !g_callback) return;
    
    JNIEnv *env;
    int attached = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
            LOGE("Failed to attach thread");
            return;
        }
        attached = 1;
    }
    
    jclass cls = (*env)->GetObjectClass(env, g_callback);
    jmethodID mid = (*env)->GetMethodID(env, cls, "onDataDecoded", "(Ljava/lang/String;)V");
    
    if (mid) {
        jstring jdata = (*env)->NewStringUTF(env, data);
        (*env)->CallVoidMethod(env, g_callback, mid, jdata);
        (*env)->DeleteLocalRef(env, jdata);
    }
    
    (*env)->DeleteLocalRef(env, cls);
    
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

// FM demodulation state
static int fm_pre_r = 0, fm_pre_j = 0;
static int fm_dc_avg = 0;
static int fm_downsample = 0;
static int fm_now_r = 0, fm_now_j = 0;
static int fm_prev_index = 0;

// Sample processing state (need to be resettable)
static int proc_callback_count = 0;
static int proc_sample_count = 0;
static unsigned int proc_overlap_samples = 0;

// Overlap buffer for AFSK correlation - persists between process_samples calls
static float overlap_buf[64];  // Max overlap needed
static int overlap_cnt = 0;

// Reset all processing state
static void reset_processing_state(void) {
    fm_pre_r = 0;
    fm_pre_j = 0;
    fm_dc_avg = 0;
    fm_downsample = 0;
    fm_now_r = 0;
    fm_now_j = 0;
    fm_prev_index = 0;
    proc_callback_count = 0;
    proc_sample_count = 0;
    proc_overlap_samples = 0;
    overlap_cnt = 0;
    memset(overlap_buf, 0, sizeof(overlap_buf));
}

// Polar discriminant for FM demodulation (from rtl_input.c)
static int polar_discriminant(int ar, int aj, int br, int bj) {
    int cr, cj;
    double angle;
    cr = ar*br + aj*bj;
    cj = aj*br - ar*bj;
    angle = atan2((double)cj, (double)cr);
    return (int)(angle / M_PI * (1<<14));
}

// Simple low pass filter with accumulation and downsample (from rtl_fm.c)
// This matches rtl_fm default behavior - no FIR filtering needed
static void low_pass(int16_t *lowpassed, int *lp_len, int downsample) {
    int i = 0, i2 = 0;
    
    while (i < *lp_len) {
        fm_now_r += lowpassed[i];
        fm_now_j += lowpassed[i+1];
        i += 2;
        fm_prev_index++;
        if (fm_prev_index < downsample) {
            continue;
        }
        lowpassed[i2]   = fm_now_r;
        lowpassed[i2+1] = fm_now_j;
        fm_prev_index = 0;
        fm_now_r = 0;
        fm_now_j = 0;
        i2 += 2;
    }
    *lp_len = i2;
}

// Process received samples through multimon
void process_samples(unsigned char *buf, uint32_t len) {
    static int16_t lp_buf[32768];  // lowpass buffer
    
    if (len == 0) return;
    
    proc_callback_count++;
    proc_sample_count += len;
    
    // Note: rotate_90 is only needed when using offset tuning (freq + sample_rate/4)
    // RTL-TCP servers send samples at the exact requested frequency, so we skip rotate_90
    // For USB mode with offset tuning, rotate_90 would shift the signal back to baseband
    // Since we're not using offset tuning, we process samples directly
    
    // Send status updates to Flutter every 500 callbacks
    if (proc_callback_count % 500 == 0) {
        char status[256];
        snprintf(status, sizeof(status), 
                "[Status] Processed %d buffers, %.1f MB of samples, listening...\n", 
                proc_callback_count, proc_sample_count / 1000000.0);
        _verbprintf(0, "%s", status);
    }
    
    // Initialize downsample factor based on sample rate
    // multimon expects 22050 Hz
    if (fm_downsample == 0) {
        uint32_t rtl_rate = 0;
        if (tcp_mode) {
            rtl_rate = tcp_sample_rate;
        } else if (dev) {
            rtl_rate = rtlsdr_get_sample_rate(dev);
        }
        
        if (rtl_rate > 0) {
            // Simple downsample calculation - matches rtl_fm default behavior
            fm_downsample = rtl_rate / 22050;
            if (fm_downsample < 1) fm_downsample = 1;
            
            int actual_rate = rtl_rate / fm_downsample;
            LOGI("FM demod: input rate=%u, downsample=%d, actual output=%d Hz (target=22050)", 
                 rtl_rate, fm_downsample, actual_rate);
        } else {
            fm_downsample = 10;  // fallback for 220500 Hz
            LOGI("FM demod: using fallback downsample=%d", fm_downsample);
        }
    }
    
    // Convert unsigned 8-bit IQ to signed 16-bit
    int lp_len = len;
    for (int i = 0; i < (int)len; i++) {
        lp_buf[i] = (int16_t)buf[i] - 127;
    }
    
    if (lp_len < 4) return;  // need at least 2 IQ samples
    
    // Step 1: Simple low_pass - matches rtl_fm default behavior (no FIR filtering)
    low_pass(lp_buf, &lp_len, fm_downsample);
    
    if (lp_len < 4) return;
    
    // Step 2: FM demodulation using polar discriminant
    int out_len = 0;
    int16_t result[16384];
    
    // First sample uses previous values
    result[out_len++] = (int16_t)polar_discriminant(lp_buf[0], lp_buf[1], fm_pre_r, fm_pre_j);
    
    // Remaining samples
    for (int i = 2; i < lp_len - 1; i += 2) {
        result[out_len++] = (int16_t)polar_discriminant(lp_buf[i], lp_buf[i+1], lp_buf[i-2], lp_buf[i-1]);
    }
    
    // Save last IQ for next buffer
    fm_pre_r = lp_buf[lp_len - 2];
    fm_pre_j = lp_buf[lp_len - 1];
    
    // Step 3: DC block filter
    int64_t sum = 0;
    int max_val = 0, min_val = 0;
    for (int i = 0; i < out_len; i++) {
        sum += result[i];
        if (result[i] > max_val) max_val = result[i];
        if (result[i] < min_val) min_val = result[i];
    }
    int avg = (int)(sum / out_len);
    avg = (avg + fm_dc_avg * 9) / 10;
    for (int i = 0; i < out_len; i++) {
        result[i] -= avg;
    }
    fm_dc_avg = avg;
    
    // Log signal levels periodically for debugging
    if (proc_callback_count % 100 == 1) {
        LOGI("FM demod: %d samples, level min=%d max=%d dc=%d", out_len, min_val, max_val, avg);
    }
    
    // Step 4: Convert to float for multimon (-1.0 to 1.0 range)
    // polar_discriminant outputs range of approximately +/- 16384
    static float fbuf[16384 + 64];  // Float buffer with room for overlap
    int fbuf_cnt = 0;
    
    // Copy overlap from previous call to beginning
    for (int i = 0; i < overlap_cnt; i++) {
        fbuf[fbuf_cnt++] = overlap_buf[i];
    }
    
    // Add new samples
    for (int i = 0; i < out_len && fbuf_cnt < 16384; i++) {
        fbuf[fbuf_cnt++] = result[i] / 16384.0f;
    }
    
    // Get the required overlap for AFSK1200 (CORRLEN = 18)
    int overlap = (int)proc_overlap_samples;
    if (overlap < 18) overlap = 18;  // Minimum for AFSK1200
    
    // Only process if we have more samples than overlap
    if (fbuf_cnt > overlap) {
        int process_len = fbuf_cnt - overlap;
        
        // Process through all active demodulators
        buffer_t buffer = { .fbuffer = fbuf, .sbuffer = NULL };
        
        for (int i = 0; i < num_demods; i++) {
            if (demod_states[i] && demod_params[i] && demod_params[i]->demod) {
                if (proc_callback_count == 1) {
                    LOGI("Calling demod %d: %s with %d samples (overlap=%d)", 
                         i, demod_params[i]->name, process_len, overlap);
                }
                demod_params[i]->demod(demod_states[i], buffer, process_len);
            }
        }
        
        // Move overlap samples to beginning for next iteration
        memmove(fbuf, fbuf + process_len, overlap * sizeof(float));
        // Save overlap for next call
        overlap_cnt = overlap;
        memcpy(overlap_buf, fbuf, overlap * sizeof(float));
    } else {
        // Not enough samples yet, save all for next time
        overlap_cnt = fbuf_cnt;
        memcpy(overlap_buf, fbuf, fbuf_cnt * sizeof(float));
    }
}

// RTL-SDR async callback
void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    if (running) {
        process_samples(buf, len);
    }
}

// USB Read thread
void *read_thread_fn(void *arg) {
    LOGI("Starting RTL-SDR USB read thread");
    rtlsdr_read_async(dev, rtlsdr_callback, NULL, 0, 0);
    LOGI("RTL-SDR USB read thread exited");
    return NULL;
}

// RTL-TCP protocol commands
#define RTL_TCP_SET_FREQ     0x01
#define RTL_TCP_SET_SAMPLE   0x02
#define RTL_TCP_SET_GAIN_MODE 0x03
#define RTL_TCP_SET_GAIN     0x04
#define RTL_TCP_SET_AGC      0x08

static int tcp_send_command(uint8_t cmd, uint32_t param) {
    if (tcp_socket < 0) return -1;
    
    uint8_t buf[5];
    buf[0] = cmd;
    buf[1] = (param >> 24) & 0xff;
    buf[2] = (param >> 16) & 0xff;
    buf[3] = (param >> 8) & 0xff;
    buf[4] = param & 0xff;
    
    if (send(tcp_socket, buf, 5, 0) != 5) {
        LOGE("TCP send command failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// TCP Read thread
void *tcp_read_thread_fn(void *arg) {
    LOGI("Starting RTL-TCP read thread");
    
    unsigned char buf[16384];
    int bytes_read;
    
    // Skip the 12-byte dongle info header
    bytes_read = recv(tcp_socket, buf, 12, MSG_WAITALL);
    if (bytes_read == 12) {
        LOGI("RTL-TCP dongle info: magic=%c%c%c%c tuner=%d gain_count=%d",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    }
    
    while (running && tcp_socket >= 0) {
        bytes_read = recv(tcp_socket, buf, sizeof(buf), 0);
        if (bytes_read > 0) {
            process_samples(buf, bytes_read);
        } else if (bytes_read == 0) {
            LOGI("RTL-TCP connection closed");
            break;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOGE("RTL-TCP recv error: %s", strerror(errno));
                break;
            }
        }
    }
    
    LOGI("RTL-TCP read thread exited");
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    LOGI("JNI OnLoad called");
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_getDeviceCount(JNIEnv *env, jobject thiz) {
    return rtlsdr_get_device_count();
}

JNIEXPORT jstring JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_getDeviceName(JNIEnv *env, jobject thiz, jint index) {
    const char *name = rtlsdr_get_device_name(index);
    return name ? (*env)->NewStringUTF(env, name) : NULL;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_openDevice(JNIEnv *env, jobject thiz, jint index) {
    if (dev) {
        LOGE("Device already open");
        return -1;
    }
    
    int ret = rtlsdr_open(&dev, index);
    if (ret < 0) {
        LOGE("Failed to open RTL-SDR device: %d", ret);
        return ret;
    }
    
    LOGI("RTL-SDR device opened successfully");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_openDeviceWithFd(JNIEnv *env, jobject thiz, jint fd, jstring path) {
    if (dev) {
        LOGE("Device already open");
        return -1;
    }
    
    const char *path_str = (*env)->GetStringUTFChars(env, path, NULL);
    
    LOGI("Opening RTL-SDR with FD=%d, path=%s", fd, path_str);
    
    // Use Android-specific open function with file descriptor
    extern int rtlsdr_open2(rtlsdr_dev_t **out_dev, int fd, const char *devicePath);
    int ret = rtlsdr_open2(&dev, fd, path_str);
    
    (*env)->ReleaseStringUTFChars(env, path, path_str);
    
    if (ret < 0) {
        LOGE("Failed to open RTL-SDR device with FD: %d", ret);
        return ret;
    }
    
    LOGI("RTL-SDR device opened successfully with FD");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_closeDevice(JNIEnv *env, jobject thiz) {
    if (dev) {
        if (running) {
            running = 0;
            rtlsdr_cancel_async(dev);
            pthread_join(read_thread, NULL);
        }
        
        // Clean up demodulators
        for (int i = 0; i < num_demods; i++) {
            if (demod_states[i]) {
                if (demod_params[i] && demod_params[i]->deinit) {
                    demod_params[i]->deinit(demod_states[i]);
                }
                free(demod_states[i]);
                demod_states[i] = NULL;
                demod_params[i] = NULL;
            }
        }
        num_demods = 0;
        
        rtlsdr_close(dev);
        dev = NULL;
        LOGI("RTL-SDR device closed");
    }
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setFrequency(JNIEnv *env, jobject thiz, jint freq) {
    if (!dev) return -1;
    return rtlsdr_set_center_freq(dev, freq);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setSampleRate(JNIEnv *env, jobject thiz, jint rate) {
    if (!dev) return -1;
    return rtlsdr_set_sample_rate(dev, rate);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setGain(JNIEnv *env, jobject thiz, jint gain) {
    if (!dev) return -1;
    rtlsdr_set_tuner_gain_mode(dev, 1);
    return rtlsdr_set_tuner_gain(dev, gain);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_addDemodulator(JNIEnv *env, jobject thiz, jstring type) {
    if (num_demods >= 16) {
        LOGE("Maximum demodulators reached");
        return -1;
    }
    
    const char *type_str = (*env)->GetStringUTFChars(env, type, NULL);
    
    const struct demod_param *param = NULL;
    
    // Select demodulator type
    if (strcmp(type_str, "POCSAG512") == 0) {
        param = &demod_poc5;
        LOGI("Adding POCSAG512 demodulator");
    } else if (strcmp(type_str, "POCSAG1200") == 0) {
        param = &demod_poc12;
        LOGI("Adding POCSAG1200 demodulator");
    } else if (strcmp(type_str, "POCSAG2400") == 0) {
        param = &demod_poc24;
        LOGI("Adding POCSAG2400 demodulator");
    } else if (strcmp(type_str, "FLEX") == 0) {
        param = &demod_flex;
        LOGI("Adding FLEX demodulator");
    } else if (strcmp(type_str, "EAS") == 0) {
        param = &demod_eas;
        LOGI("Adding EAS demodulator");
    } else if (strcmp(type_str, "AFSK1200") == 0) {
        param = &demod_afsk1200;
        LOGI("Adding AFSK1200 demodulator (APRS)");
    } else if (strcmp(type_str, "AFSK2400") == 0) {
        param = &demod_afsk2400;
        LOGI("Adding AFSK2400 demodulator");
    } else {
        LOGE("Unknown demodulator type: %s", type_str);
        (*env)->ReleaseStringUTFChars(env, type, type_str);
        return -1;
    }
    
    (*env)->ReleaseStringUTFChars(env, type, type_str);
    
    // Allocate and initialize demodulator state
    struct demod_state *s = (struct demod_state *)malloc(sizeof(struct demod_state));
    if (!s) {
        LOGE("Failed to allocate demodulator state");
        return -1;
    }
    
    memset(s, 0, sizeof(struct demod_state));
    s->dem_par = param;
    
    if (param->init) {
        param->init(s);
    }
    
    demod_states[num_demods] = s;
    demod_params[num_demods] = param;
    num_demods++;
    
    // Update overlap to maximum of all demodulators
    if (param->overlap > proc_overlap_samples) {
        proc_overlap_samples = param->overlap;
        LOGI("Updated overlap to %u samples for %s", proc_overlap_samples, param->name);
    }
    
    return num_demods - 1;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_startReceiving(JNIEnv *env, jobject thiz, jobject callback) {
    if (!dev || running) {
        LOGE("Device not open or already running");
        return;
    }
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
    }
    g_callback = (*env)->NewGlobalRef(env, callback);
    
    // Enable APRS mode for proper packet decoding
    aprs_mode = 1;
    
    // Reset all processing state
    reset_processing_state();
    
    // Print multimon-ng startup banner
    LOGI("Starting multimon-ng");
    _verbprintf(0, "multimon-ng 1.4.1\n");
    _verbprintf(0, "  (C) 1996/1997 by Tom Sailer HB9JNX/AE4WA\n");
    _verbprintf(0, "  (C) 2012-2025 by Elias Oenal\n");
    
    // List available demodulators
    _verbprintf(0, "Available demodulators: POCSAG512 POCSAG1200 POCSAG2400 FLEX EAS AFSK1200 AFSK2400 DTMF ZVEI and more\n");
    
    // Show enabled demodulators
    _verbprintf(0, "Enabled demodulators:");
    for (int i = 0; i < num_demods; i++) {
        if (demod_params[i]) {
            _verbprintf(0, " %s", demod_params[i]->name);
        }
    }
    _verbprintf(0, "\n");
    
    // Show RTL-SDR configuration
    uint32_t freq = rtlsdr_get_center_freq(dev);
    uint32_t rate = rtlsdr_get_sample_rate(dev);
    _verbprintf(0, "Using RTL-SDR input\n");
    _verbprintf(0, "Frequency: %u Hz\n", freq);
    _verbprintf(0, "Sample rate: %u Hz (FM demod to 22050 Hz)\n", rate);
    _verbprintf(0, "RTL-SDR running, waiting for signals...\n");
    
    rtlsdr_reset_buffer(dev);
    running = 1;
    
    if (pthread_create(&read_thread, NULL, read_thread_fn, NULL) != 0) {
        LOGE("Failed to create read thread");
        running = 0;
        return;
    }
    
    LOGI("Started receiving");
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_stopReceiving(JNIEnv *env, jobject thiz) {
    if (running) {
        running = 0;
        
        if (tcp_mode && tcp_socket >= 0) {
            // Close TCP socket to unblock recv()
            shutdown(tcp_socket, SHUT_RDWR);
            close(tcp_socket);
            tcp_socket = -1;
            pthread_join(read_thread, NULL);
            LOGI("Stopped TCP receiving");
        } else if (dev) {
            rtlsdr_cancel_async(dev);
            pthread_join(read_thread, NULL);
            LOGI("Stopped USB receiving");
        }
    }
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
        g_callback = NULL;
    }
}

// ============== RTL-TCP Support ==============

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_connectTcp(JNIEnv *env, jobject thiz, jstring host, jint port) {
    if (tcp_socket >= 0 || dev != NULL) {
        LOGE("Already connected");
        return -1;
    }
    
    const char *host_str = (*env)->GetStringUTFChars(env, host, NULL);
    LOGI("Connecting to RTL-TCP server %s:%d", host_str, port);
    
    // Create socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        (*env)->ReleaseStringUTFChars(env, host, host_str);
        return -1;
    }
    
    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(tcp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Connect
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host_str, &server_addr.sin_addr) <= 0) {
        LOGE("Invalid address: %s", host_str);
        close(tcp_socket);
        tcp_socket = -1;
        (*env)->ReleaseStringUTFChars(env, host, host_str);
        return -1;
    }
    
    if (connect(tcp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOGE("Connection failed: %s", strerror(errno));
        close(tcp_socket);
        tcp_socket = -1;
        (*env)->ReleaseStringUTFChars(env, host, host_str);
        return -1;
    }
    
    // Remove timeout for streaming
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(tcp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    tcp_mode = 1;
    LOGI("Connected to RTL-TCP server %s:%d", host_str, port);
    (*env)->ReleaseStringUTFChars(env, host, host_str);
    
    return 0;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_disconnectTcp(JNIEnv *env, jobject thiz) {
    if (running) {
        running = 0;
        if (tcp_socket >= 0) {
            shutdown(tcp_socket, SHUT_RDWR);
            close(tcp_socket);
            tcp_socket = -1;
            pthread_join(read_thread, NULL);
        }
    }
    
    if (tcp_socket >= 0) {
        close(tcp_socket);
        tcp_socket = -1;
    }
    
    // Clean up demodulators
    for (int i = 0; i < num_demods; i++) {
        if (demod_states[i]) {
            if (demod_params[i] && demod_params[i]->deinit) {
                demod_params[i]->deinit(demod_states[i]);
            }
            free(demod_states[i]);
            demod_states[i] = NULL;
            demod_params[i] = NULL;
        }
    }
    num_demods = 0;
    
    tcp_mode = 0;
    LOGI("Disconnected from RTL-TCP");
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
        g_callback = NULL;
    }
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpFrequency(JNIEnv *env, jobject thiz, jint freq) {
    tcp_frequency = freq;
    if (tcp_socket >= 0) {
        return tcp_send_command(RTL_TCP_SET_FREQ, freq);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpSampleRate(JNIEnv *env, jobject thiz, jint rate) {
    tcp_sample_rate = rate;
    if (tcp_socket >= 0) {
        return tcp_send_command(RTL_TCP_SET_SAMPLE, rate);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpGain(JNIEnv *env, jobject thiz, jint gain) {
    if (tcp_socket >= 0) {
        tcp_send_command(RTL_TCP_SET_GAIN_MODE, 1);  // Manual gain
        return tcp_send_command(RTL_TCP_SET_GAIN, gain);
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_startTcpReceiving(JNIEnv *env, jobject thiz, jobject callback) {
    if (tcp_socket < 0 || running) {
        LOGE("TCP not connected or already running");
        return;
    }
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
    }
    g_callback = (*env)->NewGlobalRef(env, callback);
    
    // Enable APRS mode for proper packet decoding
    aprs_mode = 1;
    
    // Reset all processing state
    reset_processing_state();
    
    // Print multimon-ng startup banner
    LOGI("Starting multimon-ng via RTL-TCP");
    _verbprintf(0, "multimon-ng 1.4.1\n");
    _verbprintf(0, "  (C) 1996/1997 by Tom Sailer HB9JNX/AE4WA\n");
    _verbprintf(0, "  (C) 2012-2025 by Elias Oenal\n");
    
    _verbprintf(0, "Available demodulators: POCSAG512 POCSAG1200 POCSAG2400 FLEX EAS AFSK1200 AFSK2400 DTMF ZVEI and more\n");
    
    _verbprintf(0, "Enabled demodulators:");
    for (int i = 0; i < num_demods; i++) {
        if (demod_params[i]) {
            _verbprintf(0, " %s", demod_params[i]->name);
        }
    }
    _verbprintf(0, "\n");
    
    _verbprintf(0, "Using RTL-TCP input\n");
    _verbprintf(0, "Frequency: %u Hz\n", tcp_frequency);
    _verbprintf(0, "Sample rate: %u Hz (FM demod to 22050 Hz)\n", tcp_sample_rate);
    
    // Enable automatic gain control
    tcp_send_command(RTL_TCP_SET_GAIN_MODE, 0);  // 0 = manual mode
    tcp_send_command(RTL_TCP_SET_GAIN, 400);     // Gain in tenths of dB (40.0 dB)
    _verbprintf(0, "Set gain to 40.0 dB (manual mode)\n");
    
    _verbprintf(0, "RTL-TCP running, waiting for signals...\n");
    
    running = 1;
    
    if (pthread_create(&read_thread, NULL, tcp_read_thread_fn, NULL) != 0) {
        LOGE("Failed to create TCP read thread");
        running = 0;
        return;
    }
    
    LOGI("Started TCP receiving");
}

JNIEXPORT jboolean JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_isTcpConnected(JNIEnv *env, jobject thiz) {
    return tcp_socket >= 0 && tcp_mode;
}

