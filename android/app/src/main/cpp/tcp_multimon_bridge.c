/*
 * tcp_multimon_bridge.c - Simple TCP client to multimon-ng bridge
 *
 * Connects to SDR++ on localhost:7355 to receive raw audio data and feeds it to multimon-ng
 * Usage: SDR++ runs TCP server on port 7355, this app connects as client
 */

#include <jni.h>
#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "multimon/multimon_lib.h"

#define LOG_TAG "TcpMultimonBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

#define TCP_PORT 7355
#define AUDIO_SAMPLE_RATE 22050
#define BUFFER_SIZE 8192

/* State */
static int client_socket = -1;
static pthread_t client_thread;
static int running = 0;
static multimon_ctx_t* multimon = NULL;

/* JNI callback */
static JavaVM *g_jvm = NULL;
static jobject g_callback = NULL;

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
        // Format: [decoder] message
        char formatted[1024];
        snprintf(formatted, sizeof(formatted), "[%s] %s", decoder_name, message);
        
        jstring jdata = (*env)->NewStringUTF(env, formatted);
        (*env)->CallVoidMethod(env, g_callback, mid, jdata);
        (*env)->DeleteLocalRef(env, jdata);
    }
    
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/* TCP client thread - connects to SDR++ and reads audio */
static void* client_thread_fn(void* arg) {
    (void)arg;
    
    LOGI("TCP client thread starting");
    
    int16_t buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while (running) {
        // Connect to SDR++ server
        LOGI("Connecting to localhost:%d...", TCP_PORT);
        
        client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket < 0) {
            LOGE("socket failed: %s", strerror(errno));
            sleep(5);
            continue;
        }
        
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        server_addr.sin_port = htons(TCP_PORT);
        
        if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            LOGE("connect failed: %s (is SDR++ running?)", strerror(errno));
            close(client_socket);
            client_socket = -1;
            sleep(5);
            continue;
        }
        
        LOGI("Connected to SDR++ server, receiving audio...");
        
        // Read audio data and feed to multimon
        size_t total_samples = 0;
        while (running) {
            bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
            
            if (bytes_read <= 0) {
                if (bytes_read < 0) {
                    LOGE("recv error: %s", strerror(errno));
                } else {
                    LOGI("Server disconnected");
                }
                break;
            }
            
            // Process samples through multimon
            size_t samples = bytes_read / sizeof(int16_t);
            total_samples += samples;
            
            // Log every 10 seconds of audio
            if (total_samples >= (AUDIO_SAMPLE_RATE * 10)) {
                LOGD("Processed %zu samples (%.1f seconds)", total_samples, (float)total_samples / AUDIO_SAMPLE_RATE);
                total_samples = 0;
            }
            
            if (multimon) {
                multimon_process(multimon, buffer, samples);
            }
        }
        
        close(client_socket);
        client_socket = -1;
        
        if (running) {
            LOGI("Connection lost, reconnecting in 5 seconds...");
            sleep(5);
        }
    }
    
    LOGI("TCP client thread exiting");
    return NULL;
}

/* JNI Functions */

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    LOGI("TcpMultimonBridge loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_TcpMultimonService_startClient(JNIEnv *env, jobject thiz, jobject callback) {
    (void)thiz;
    
    LOGI("Starting TCP client to connect to localhost:%d", TCP_PORT);
    
    if (running) {
        LOGE("Client already running");
        return -1;
    }
    
    // Store callback
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
    }
    g_callback = (*env)->NewGlobalRef(env, callback);
    
    // Create multimon context
    multimon = multimon_create(multimon_decoded_callback, NULL, AUDIO_SAMPLE_RATE);
    if (!multimon) {
        LOGE("Failed to create multimon context");
        return -1;
    }
    
    // Enable FLEX decoder by default
    if (multimon_enable_decoder(multimon, MULTIMON_FLEX) != 0) {
        LOGE("Failed to enable FLEX decoder");
        multimon_destroy(multimon);
        multimon = NULL;
        return -1;
    }
    
    LOGI("Multimon initialized with FLEX decoder (sample rate: %d)", AUDIO_SAMPLE_RATE);
    
    // Start client thread
    running = 1;
    if (pthread_create(&client_thread, NULL, client_thread_fn, NULL) != 0) {
        LOGE("pthread_create failed");
        running = 0;
        multimon_destroy(multimon);
        multimon = NULL;
        return -1;
    }
    
    LOGI("TCP client started successfully");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_TcpMultimonService_stopClient(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    
    LOGI("Stopping TCP client");
    
    if (!running) {
        LOGE("Client not running");
        return -1;
    }
    
    running = 0;
    
    // Close socket to break recv()
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
    
    // Wait for thread to exit
    pthread_join(client_thread, NULL);
    
    // Cleanup multimon
    if (multimon) {
        multimon_destroy(multimon);
        multimon = NULL;
    }
    
    // Cleanup callback
    if (g_callback) {
        (*env)->DeleteGlobalRef(env, g_callback);
        g_callback = NULL;
    }
    
    LOGI("TCP client stopped");
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_com_multimon_multimon_1android_TcpMultimonService_isRunning(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    return running ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_multimon_multimon_1android_TcpMultimonService_enableDecoder(JNIEnv *env, jobject thiz, jstring decoder_name) {
    (void)thiz;
    
    if (!multimon) {
        LOGE("Multimon not initialized");
        return -1;
    }
    
    const char* name = (*env)->GetStringUTFChars(env, decoder_name, NULL);
    
    multimon_decoder_t decoder;
    int found = 1;
    
    // Map string to decoder enum
    if (strcmp(name, "FLEX") == 0) {
        decoder = MULTIMON_FLEX;
    } else if (strcmp(name, "POCSAG512") == 0) {
        decoder = MULTIMON_POCSAG512;
    } else if (strcmp(name, "POCSAG1200") == 0) {
        decoder = MULTIMON_POCSAG1200;
    } else if (strcmp(name, "POCSAG2400") == 0) {
        decoder = MULTIMON_POCSAG2400;
    } else if (strcmp(name, "AFSK1200") == 0) {
        decoder = MULTIMON_AFSK1200;
    } else if (strcmp(name, "AFSK2400") == 0) {
        decoder = MULTIMON_AFSK2400;
    } else if (strcmp(name, "EAS") == 0) {
        decoder = MULTIMON_EAS;
    } else if (strcmp(name, "DTMF") == 0) {
        decoder = MULTIMON_DTMF;
    } else if (strcmp(name, "ZVEI1") == 0) {
        decoder = MULTIMON_ZVEI1;
    } else if (strcmp(name, "ZVEI2") == 0) {
        decoder = MULTIMON_ZVEI2;
    } else if (strcmp(name, "ZVEI3") == 0) {
        decoder = MULTIMON_ZVEI3;
    } else if (strcmp(name, "MORSE") == 0) {
        decoder = MULTIMON_MORSE;
    } else if (strcmp(name, "UFSK1200") == 0) {
        decoder = MULTIMON_UFSK1200;
    } else if (strcmp(name, "CLIPFSK") == 0) {
        decoder = MULTIMON_CLIPFSK;
    } else if (strcmp(name, "FMSFSK") == 0) {
        decoder = MULTIMON_FMSFSK;
    } else if (strcmp(name, "DZVEI") == 0) {
        decoder = MULTIMON_DZVEI;
    } else if (strcmp(name, "PZVEI") == 0) {
        decoder = MULTIMON_PZVEI;
    } else if (strcmp(name, "EEA") == 0) {
        decoder = MULTIMON_EEA;
    } else if (strcmp(name, "EIA") == 0) {
        decoder = MULTIMON_EIA;
    } else if (strcmp(name, "CCIR") == 0) {
        decoder = MULTIMON_CCIR;
    } else if (strcmp(name, "HAPN4800") == 0) {
        decoder = MULTIMON_HAPN4800;
    } else if (strcmp(name, "FSK9600") == 0) {
        decoder = MULTIMON_FSK9600;
    } else if (strcmp(name, "X10") == 0) {
        decoder = MULTIMON_X10;
    } else {
        LOGE("Unknown decoder: %s", name);
        (*env)->ReleaseStringUTFChars(env, decoder_name, name);
        return -1;
    }
    
    int ret = multimon_enable_decoder(multimon, decoder);
    if (ret == 0) {
        LOGI("Decoder %s enabled", name);
    } else {
        LOGE("Failed to enable decoder %s", name);
    }
    
    (*env)->ReleaseStringUTFChars(env, decoder_name, name);
    return ret;
}
