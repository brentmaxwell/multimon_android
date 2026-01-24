/*
 * jni_bridge.c - New JNI bridge for multimon-android
 *
 * This replaces the old jni_wrapper.c with a clean architecture:
 * - RTL-SDR (USB/TCP) provides IQ samples
 * - FM demodulator converts IQ to audio
 * - Audio pipe connects FM demod to multimon
 * - Multimon decodes protocols (AFSK, POCSAG, etc.)
 */

#include <jni.h>
#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "rtlsdr/librtlsdr/include/rtl-sdr.h"
#include "rtl_fm/fm_demod.h"
#include "audio_pipe/audio_pipe.h"
#include "multimon/multimon_lib.h"

#define LOG_TAG "MultimonRTLSDR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/* RTL-SDR device */
static rtlsdr_dev_t *dev = NULL;

/* RTL-TCP connection */
static int tcp_socket = -1;
static int tcp_mode = 0;  /* 0 = USB, 1 = TCP */
static uint32_t tcp_sample_rate = 220500;  /* 22050 * 10 for exact division */
static uint32_t tcp_frequency = 144390000;

/* Processing components */
static fm_demod_t* fm_demod = NULL;
static audio_pipe_t* audio_pipe = NULL;
static multimon_ctx_t* multimon = NULL;

/* Audio output for debugging */
static audio_pipe_t* audio_output_pipe = NULL;
static int audio_output_enabled = 0;

/* Threading */
static pthread_t read_thread;
static int running = 0;

/* JNI callback */
static JavaVM *g_jvm = NULL;
static jobject g_callback = NULL;

/* Buffer sizes */
#define IQ_BUFFER_SIZE 16384
#define AUDIO_PIPE_SIZE (22050 * 2)  /* 2 seconds of audio */
#define AUDIO_OUTPUT_PIPE_SIZE (22050 * 4)  /* 4 seconds buffer for playback */

/* RTL-TCP commands */
#define RTL_TCP_SET_FREQ     0x01
#define RTL_TCP_SET_SRATE    0x02
#define RTL_TCP_SET_GAIN_MODE 0x03
#define RTL_TCP_SET_GAIN     0x04
#define RTL_TCP_SET_IF_GAIN  0x06
#define RTL_TCP_SET_AGC_MODE 0x08

/* Multimon callback - called when packets are decoded */
static void multimon_decoded_callback(const char* decoder_name, const char* message, void* user_data) {
    (void)user_data;
    
    LOGI("[%s] %s", decoder_name, message);
    
    if (!g_jvm || !g_callback) return;
    
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
        jstring jdata = (*env)->NewStringUTF(env, message);
        (*env)->CallVoidMethod(env, g_callback, mid, jdata);
        (*env)->DeleteLocalRef(env, jdata);
    }
    
    (*env)->DeleteLocalRef(env, cls);
    
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/* Process IQ samples through FM demod and multimon */
static int process_count = 0;
static void process_iq_samples(uint8_t* buf, int len) {
    if (!fm_demod || !audio_pipe || !multimon) {
        LOGD("process_iq_samples: components not ready");
        return;
    }
    
    /* FM demodulate IQ samples to audio */
    /* For 220500 Hz input to 22050 Hz output with 10x decimation, 
       max output is len/2 (IQ pairs) / 10 (decimation) = len/20 */
    static int16_t audio_out[16384];  /* Static buffer to avoid stack issues */
    size_t max_out = sizeof(audio_out) / sizeof(audio_out[0]);
    
    size_t audio_samples = fm_demod_process(fm_demod, buf, len, audio_out, max_out);
    
    process_count++;
    
    /* Debug: log audio output periodically */
    if (process_count <= 5 || process_count % 1000 == 0) {
        if (audio_samples > 0) {
            /* Check audio levels */
            int min_val = 32767, max_val = -32768;
            for (size_t i = 0; i < audio_samples && i < 100; i++) {
                if (audio_out[i] < min_val) min_val = audio_out[i];
                if (audio_out[i] > max_val) max_val = audio_out[i];
            }
            LOGI("FM demod: %zu audio samples, level range [%d, %d]", 
                 audio_samples, min_val, max_val);
        } else {
            LOGD("FM demod: no audio output from %d IQ bytes", len);
        }
    }
    
    if (audio_samples > 0) {
        /* Write audio to pipe for multimon */
        audio_pipe_write(audio_pipe, audio_out, audio_samples);
        
        /* Also write to audio output pipe if enabled */
        if (audio_output_enabled && audio_output_pipe) {
            audio_pipe_write(audio_output_pipe, audio_out, audio_samples);
        }
        
        /* Read from pipe and process through multimon */
        int16_t multimon_buf[1024];
        size_t read_samples;
        int chunks_processed = 0;
        
        while ((read_samples = audio_pipe_read(audio_pipe, multimon_buf, 1024)) > 0) {
            multimon_process(multimon, multimon_buf, read_samples);
            chunks_processed++;
        }
        
        /* Debug: log multimon processing periodically */
        if (process_count <= 5 || process_count % 1000 == 0) {
            LOGI("Multimon: processed %d chunks from pipe", chunks_processed);
        }
    }
}

/* RTL-TCP command sending */
static int tcp_send_command(uint8_t cmd, uint32_t param) {
    if (tcp_socket < 0) return -1;
    
    uint8_t buffer[5];
    buffer[0] = cmd;
    buffer[1] = (param >> 24) & 0xFF;
    buffer[2] = (param >> 16) & 0xFF;
    buffer[3] = (param >> 8) & 0xFF;
    buffer[4] = param & 0xFF;
    
    int ret = send(tcp_socket, buffer, 5, 0);
    LOGD("TCP command: cmd=%d param=%u ret=%d", cmd, param, ret);
    return ret;
}

/* TCP read thread */
static void* tcp_read_thread_fn(void* arg) {
    (void)arg;
    
    uint8_t buffer[IQ_BUFFER_SIZE];
    int bytes_read;
    long total_bytes = 0;
    int buffer_count = 0;
    
    LOGI("Starting RTL-TCP read thread, socket=%d", tcp_socket);
    
    while (running && tcp_socket >= 0) {
        bytes_read = recv(tcp_socket, buffer, sizeof(buffer), 0);
        
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                LOGE("TCP read error: %s (errno=%d)", strerror(errno), errno);
            } else {
                LOGI("TCP connection closed by server");
            }
            break;
        }
        
        total_bytes += bytes_read;
        buffer_count++;
        
        /* Log first few buffers for debugging */
        if (buffer_count <= 3) {
            LOGD("TCP buffer %d: %d bytes, first bytes: %02X %02X %02X %02X",
                 buffer_count, bytes_read, buffer[0], buffer[1], buffer[2], buffer[3]);
        }
        
        /* Process IQ samples */
        process_iq_samples(buffer, bytes_read);
        
        /* Status update every 500 buffers */
        if (buffer_count % 500 == 0) {
            LOGI("[Status] Processed %d buffers, %.1f MB of samples", 
                 buffer_count, total_bytes / (1024.0 * 1024.0));
        }
    }
    
    LOGI("RTL-TCP read thread ending (buffer_count=%d, total=%.1f MB)", 
         buffer_count, total_bytes / (1024.0 * 1024.0));
    return NULL;
}

/* RTL-SDR callback for USB mode */
static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
    (void)ctx;
    
    if (!running) return;
    
    process_iq_samples(buf, len);
}

/* USB read thread */
static void* usb_read_thread_fn(void* arg) {
    (void)arg;
    
    LOGI("Starting RTL-SDR USB read thread");
    
    rtlsdr_read_async(dev, rtlsdr_callback, NULL, 0, IQ_BUFFER_SIZE);
    
    LOGI("RTL-SDR USB read thread ending");
    return NULL;
}

/* Initialize processing chain */
static int init_processing_chain(int sample_rate) {
    /* Create FM demodulator */
    fm_demod = fm_demod_create(sample_rate, 22050, FM_MODE_FM);
    if (!fm_demod) {
        LOGE("Failed to create FM demodulator");
        return -1;
    }
    
    /* Enable rotate_90 for offset tuning compensation 
       This may be needed depending on RTL-TCP server configuration
       TESTING: Disable to see if it helps decoding */
    fm_demod_set_rotate_90(fm_demod, 0);
    
    /* Create audio pipe */
    audio_pipe = audio_pipe_create(AUDIO_PIPE_SIZE);
    if (!audio_pipe) {
        LOGE("Failed to create audio pipe");
        fm_demod_destroy(fm_demod);
        fm_demod = NULL;
        return -1;
    }
    
    /* Create multimon context */
    multimon = multimon_create(multimon_decoded_callback, NULL, 22050);
    if (!multimon) {
        LOGE("Failed to create multimon context");
        audio_pipe_destroy(audio_pipe);
        audio_pipe = NULL;
        fm_demod_destroy(fm_demod);
        fm_demod = NULL;
        return -1;
    }
    
    LOGI("Processing chain initialized: IQ -> FM demod -> audio pipe -> multimon");
    return 0;
}

/* Destroy processing chain */
static void destroy_processing_chain(void) {
    if (multimon) {
        multimon_destroy(multimon);
        multimon = NULL;
    }
    
    if (audio_pipe) {
        audio_pipe_destroy(audio_pipe);
        audio_pipe = NULL;
    }
    
    if (fm_demod) {
        fm_demod_destroy(fm_demod);
        fm_demod = NULL;
    }
}

/* ============== JNI Functions ============== */

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_getDeviceCount(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return rtlsdr_get_device_count();
}

JNIEXPORT jstring JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_getDeviceName(JNIEnv *env, jobject thiz, jint index) {
    (void)thiz;
    const char *name = rtlsdr_get_device_name(index);
    return name ? (*env)->NewStringUTF(env, name) : NULL;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_openDevice(JNIEnv *env, jobject thiz, jint index) {
    (void)env; (void)thiz;
    
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
    (void)thiz;
    
    if (dev) {
        LOGE("Device already open");
        return -1;
    }
    
    const char *path_str = (*env)->GetStringUTFChars(env, path, NULL);
    
    LOGI("Opening RTL-SDR with FD=%d, path=%s", fd, path_str);
    
    /* Use Android-specific open function with file descriptor */
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
    (void)env; (void)thiz;
    
    if (dev) {
        if (running) {
            running = 0;
            rtlsdr_cancel_async(dev);
            pthread_join(read_thread, NULL);
        }
        
        destroy_processing_chain();
        
        rtlsdr_close(dev);
        dev = NULL;
        LOGI("RTL-SDR device closed");
    }
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setFrequency(JNIEnv *env, jobject thiz, jint freq) {
    (void)env; (void)thiz;
    if (!dev) return -1;
    return rtlsdr_set_center_freq(dev, freq);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setSampleRate(JNIEnv *env, jobject thiz, jint rate) {
    (void)env; (void)thiz;
    if (!dev) return -1;
    return rtlsdr_set_sample_rate(dev, rate);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setGain(JNIEnv *env, jobject thiz, jint gain) {
    (void)env; (void)thiz;
    if (!dev) return -1;
    rtlsdr_set_tuner_gain_mode(dev, 1);
    return rtlsdr_set_tuner_gain(dev, gain);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_addDemodulator(JNIEnv *env, jobject thiz, jstring type) {
    (void)thiz;
    
    if (!multimon) {
        /* Initialize processing chain if not already done */
        if (init_processing_chain(220500) < 0) {
            return -1;
        }
    }
    
    const char *type_str = (*env)->GetStringUTFChars(env, type, NULL);
    int ret = -1;
    
    /* Map string to decoder type */
    if (strcmp(type_str, "AFSK1200") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_AFSK1200);
        multimon_set_aprs_mode(multimon, 1);  /* Enable APRS mode for AFSK1200 */
        LOGI("Adding AFSK1200 demodulator (APRS)");
    } else if (strcmp(type_str, "AFSK2400") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_AFSK2400);
        LOGI("Adding AFSK2400 demodulator");
    } else if (strcmp(type_str, "POCSAG512") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_POCSAG512);
        LOGI("Adding POCSAG512 demodulator");
    } else if (strcmp(type_str, "POCSAG1200") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_POCSAG1200);
        LOGI("Adding POCSAG1200 demodulator");
    } else if (strcmp(type_str, "POCSAG2400") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_POCSAG2400);
        LOGI("Adding POCSAG2400 demodulator");
    } else if (strcmp(type_str, "EAS") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_EAS);
        LOGI("Adding EAS demodulator");
    } else if (strcmp(type_str, "FLEX") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_FLEX);
        LOGI("Adding FLEX demodulator");
    } else if (strcmp(type_str, "DTMF") == 0) {
        ret = multimon_enable_decoder(multimon, MULTIMON_DTMF);
        LOGI("Adding DTMF demodulator");
    } else {
        LOGE("Unknown demodulator type: %s", type_str);
    }
    
    (*env)->ReleaseStringUTFChars(env, type, type_str);
    
    if (ret == 0) {
        int overlap = multimon_get_overlap(multimon);
        LOGI("Updated overlap to %d samples", overlap);
    }
    
    return ret;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_startReceiving(JNIEnv *env, jobject thiz, jobject callback) {
    (void)thiz;
    
    if (!dev || running) {
        LOGE("Device not open or already running");
        return;
    }
    
    /* Store JVM and callback */
    (*env)->GetJavaVM(env, &g_jvm);
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
    }
    g_callback = (*env)->NewGlobalRef(env, callback);
    
    /* Initialize processing chain if needed */
    if (!multimon) {
        if (init_processing_chain(220500) < 0) {
            return;
        }
    }
    
    /* Reset buffers */
    fm_demod_reset(fm_demod);
    audio_pipe_reset(audio_pipe);
    
    /* Print startup banner */
    multimon_decoded_callback("STATUS", "multimon-ng 1.4.1 (New Architecture)", NULL);
    multimon_decoded_callback("STATUS", "Using USB RTL-SDR input", NULL);
    
    running = 1;
    
    /* Start USB read thread */
    if (pthread_create(&read_thread, NULL, usb_read_thread_fn, NULL) != 0) {
        LOGE("Failed to create USB read thread");
        running = 0;
        return;
    }
    
    LOGI("Started USB receiving");
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_stopReceiving(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    
    if (running) {
        running = 0;
        
        if (tcp_mode) {
            /* TCP mode - close socket to unblock recv */
            if (tcp_socket >= 0) {
                shutdown(tcp_socket, SHUT_RDWR);
            }
        } else {
            /* USB mode - cancel async read */
            if (dev) {
                rtlsdr_cancel_async(dev);
            }
        }
        
        pthread_join(read_thread, NULL);
        LOGI("Stopped receiving");
    }
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_connectTcp(JNIEnv *env, jobject thiz, jstring host, jint port) {
    (void)thiz;
    
    if (tcp_socket >= 0) {
        LOGE("TCP already connected");
        return -1;
    }
    
    const char *host_str = (*env)->GetStringUTFChars(env, host, NULL);
    
    LOGI("Connecting to RTL-TCP server %s:%d", host_str, port);
    
    /* Create socket */
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        (*env)->ReleaseStringUTFChars(env, host, host_str);
        return -1;
    }
    
    /* Connect */
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
    
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOGE("Connection failed: %s", strerror(errno));
        close(tcp_socket);
        tcp_socket = -1;
        (*env)->ReleaseStringUTFChars(env, host, host_str);
        return -1;
    }
    
    LOGI("Connected to RTL-TCP server %s:%d", host_str, port);
    
    (*env)->ReleaseStringUTFChars(env, host, host_str);
    
    /* Set TCP_NODELAY to send commands immediately */
    int flag = 1;
    setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    tcp_mode = 1;
    
    /* Read and discard the 12-byte dongle info header */
    uint8_t dongle_info[12];
    int total_read = 0;
    while (total_read < 12) {
        int n = recv(tcp_socket, dongle_info + total_read, 12 - total_read, 0);
        if (n <= 0) {
            LOGE("Failed to read dongle info: %s", strerror(errno));
            close(tcp_socket);
            tcp_socket = -1;
            tcp_mode = 0;
            return -1;
        }
        total_read += n;
    }
    
    LOGI("RTL-TCP dongle info: magic=%c%c%c%c tuner=%d gain_count=%d",
         dongle_info[0], dongle_info[1], dongle_info[2], dongle_info[3],
         dongle_info[4], dongle_info[5]);
    
    return 0;
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_disconnectTcp(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    
    if (running) {
        running = 0;
        shutdown(tcp_socket, SHUT_RDWR);
        pthread_join(read_thread, NULL);
    }
    
    if (tcp_socket >= 0) {
        close(tcp_socket);
        tcp_socket = -1;
        LOGI("TCP disconnected");
    }
    
    tcp_mode = 0;
    
    destroy_processing_chain();
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpFrequency(JNIEnv *env, jobject thiz, jint freq) {
    (void)env; (void)thiz;
    tcp_frequency = freq;
    return tcp_send_command(RTL_TCP_SET_FREQ, freq);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpSampleRate(JNIEnv *env, jobject thiz, jint rate) {
    (void)env; (void)thiz;
    tcp_sample_rate = rate;
    return tcp_send_command(RTL_TCP_SET_SRATE, rate);
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_setTcpGain(JNIEnv *env, jobject thiz, jint gain) {
    (void)env; (void)thiz;
    tcp_send_command(RTL_TCP_SET_GAIN_MODE, 1);  /* Manual gain */
    return tcp_send_command(RTL_TCP_SET_GAIN, gain);
}

JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_startTcpReceiving(JNIEnv *env, jobject thiz, jobject callback) {
    (void)thiz;
    
    if (tcp_socket < 0 || running) {
        LOGE("TCP not connected or already running");
        return;
    }
    
    /* Store JVM and callback */
    (*env)->GetJavaVM(env, &g_jvm);
    
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
    }
    g_callback = (*env)->NewGlobalRef(env, callback);
    
    /* Initialize processing chain if needed */
    if (!multimon) {
        if (init_processing_chain(tcp_sample_rate) < 0) {
            return;
        }
    }
    
    /* Reset buffers */
    fm_demod_reset(fm_demod);
    audio_pipe_reset(audio_pipe);
    
    /* Print startup banner */
    LOGI("Starting multimon-ng via RTL-TCP");
    multimon_decoded_callback("STATUS", "multimon-ng 1.4.1 (New Architecture)", NULL);
    multimon_decoded_callback("STATUS", "Using RTL-TCP input", NULL);
    
    char freq_msg[64];
    snprintf(freq_msg, sizeof(freq_msg), "Frequency: %u Hz", tcp_frequency);
    multimon_decoded_callback("STATUS", freq_msg, NULL);
    
    char rate_msg[64];
    snprintf(rate_msg, sizeof(rate_msg), "Sample rate: %u Hz (FM demod to 22050 Hz)", tcp_sample_rate);
    multimon_decoded_callback("STATUS", rate_msg, NULL);
    
    /* Set gain */
    tcp_send_command(RTL_TCP_SET_GAIN_MODE, 1);  /* Manual gain */
    tcp_send_command(RTL_TCP_SET_GAIN, 400);     /* 40.0 dB */
    multimon_decoded_callback("STATUS", "Set gain to 40.0 dB (manual mode)", NULL);
    
    multimon_decoded_callback("STATUS", "RTL-TCP running, waiting for signals...", NULL);
    
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
    (void)env; (void)thiz;
    return tcp_socket >= 0 && tcp_mode;
}

/* Audio output functions for debugging */
JNIEXPORT void JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_enableAudioOutput(JNIEnv *env, jobject thiz, jboolean enable) {
    (void)env; (void)thiz;
    
    if (enable && !audio_output_pipe) {
        audio_output_pipe = audio_pipe_create(AUDIO_OUTPUT_PIPE_SIZE);
        LOGI("Audio output enabled");
    }
    
    audio_output_enabled = enable ? 1 : 0;
    
    if (!enable && audio_output_pipe) {
        audio_pipe_destroy(audio_output_pipe);
        audio_output_pipe = NULL;
        LOGI("Audio output disabled");
    }
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_RtlSdrService_getAudioSamples(JNIEnv *env, jobject thiz, jshortArray buffer) {
    (void)thiz;
    
    if (!audio_output_pipe || !audio_output_enabled) return 0;
    
    jsize len = (*env)->GetArrayLength(env, buffer);
    jshort* buf = (*env)->GetShortArrayElements(env, buffer, NULL);
    
    size_t read = audio_pipe_read(audio_output_pipe, buf, len);
    
    (*env)->ReleaseShortArrayElements(env, buffer, buf, 0);
    
    return (jint)read;
}
