cmake_minimum_required(VERSION 3.10.2)

project(multimon_rtlsdr)

# Disable X11 and enable max verbosity
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -DMAX_VERBOSE_LEVEL=3 -DNO_X11")
set(CMAKE_C_FLAGS_DEBUG "-g -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# RTL-SDR library
add_subdirectory(rtlsdr/librtlsdr)

# Multimon-ng sources (from fresh upstream)
file(GLOB MULTIMON_SOURCES 
    "multimon/*.c"
)

# Exclude files we don't need (generators, platform-specific, main, stub)
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*main\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*unixinput\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*gen\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*gen_.*\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*win32_.*\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*xdisplay\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*mkcostab\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*BCHCode_stub\\.c$")
list(FILTER MULTIMON_SOURCES EXCLUDE REGEX ".*bch_stub\\.c$")

# FM demodulator sources
set(FM_DEMOD_SOURCES
    "rtl_fm/fm_demod.c"
)

# Audio pipe sources
set(AUDIO_PIPE_SOURCES
    "audio_pipe/audio_pipe.c"
)

# JNI bridge (new clean architecture)
set(JNI_BRIDGE_SOURCES
    "jni_bridge.c"
)

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

# Main shared library
add_library(multimon_rtlsdr SHARED
    ${MULTIMON_SOURCES}
    ${FM_DEMOD_SOURCES}
    ${AUDIO_PIPE_SOURCES}
    ${JNI_BRIDGE_SOURCES}
)

target_include_directories(multimon_rtlsdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/multimon
    ${CMAKE_CURRENT_SOURCE_DIR}/rtl_fm
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_pipe
    ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr
    ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr/librtlsdr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr/librtlsdr/src
)

target_link_libraries(multimon_rtlsdr
    ${LOG_LIB}
    ${ANDROID_LIB}
    librtlsdr
    m
)

# TCP-based multimon library (simpler, no RTL-SDR/FM demod)
add_library(tcp_multimon SHARED
    ${MULTIMON_SOURCES}
    tcp_multimon_bridge.c
)

target_include_directories(tcp_multimon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/multimon
)

target_link_libraries(tcp_multimon
    ${LOG_LIB}
    ${ANDROID_LIB}
    m
)
