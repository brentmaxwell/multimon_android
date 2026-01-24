# Multimon-Android

A Flutter Android application that integrates RTL-SDR hardware support with multimon-ng for decoding various digital radio protocols directly on Android devices, without requiring external driver apps.

## Overview

This project combines:
- **Flutter** - Cross-platform UI framework
- **RTL-SDR** - Native USB support for RTL2832U-based SDR dongles
- **Multimon-ng** - Multi-protocol digital signal decoder

The application provides native RTL-SDR support on Android through JNI bindings, allowing direct USB access to SDR hardware without requiring the rtl_tcp_andro driver app.

## Features

- Native RTL-SDR device access via USB
- Real-time signal demodulation and decoding
- Support for multiple protocols (POCSAG, FLEX, DTMF, ZVEI, EAS, etc.)
- Simple UI for device control and message display
- No external driver apps required

## Building

```bash
cd multimon_android
flutter pub get
flutter build apk
```

## Usage

1. Connect RTL-SDR device via USB OTG
2. Grant USB permissions when prompted
3. Tap "Connect" to open the device
4. Tap "Start" to begin receiving and decoding
5. View decoded messages in the list

See full documentation in the project for more details.
