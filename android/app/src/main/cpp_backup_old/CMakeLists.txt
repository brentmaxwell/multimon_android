cmake_minimum_required(VERSION 3.10.2)

project(multimon_rtlsdr)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -DMAX_VERBOSE_LEVEL=3")
set(CMAKE_C_FLAGS_DEBUG "-g -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# RTL-SDR library
add_subdirectory(rtlsdr/librtlsdr)

# Multimon-ng sources
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

# RTL-SDR device wrapper sources (exclude these - we're using our own JNI wrapper)
# file(GLOB RTLSDR_WRAPPER_SOURCES
#     "rtlsdr/*.c"
# )

# JNI wrapper
set(JNI_WRAPPER_SOURCES
    "jni_wrapper.c"
)

find_library(LOG_LIB log)
find_library(ANDROID_LIB android)

# Main shared library
add_library(multimon_rtlsdr SHARED
    ${MULTIMON_SOURCES}
    ${JNI_WRAPPER_SOURCES}
)

target_include_directories(multimon_rtlsdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/multimon
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
