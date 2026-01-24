# Multimon-Android

A Flutter Android application that integrates multimon-ng for decoding various digital radio protocols from audio input on Android devices.

## Overview

This project combines:
- **Flutter** - Cross-platform UI framework
- **Multimon-ng** - Multi-protocol digital signal decoder
- **TCP Audio Input** - Receives audio data via TCP from SDR++ or similar applications

**Note:** This is primarily an audio decoding tool. It accepts TCP audio input from SDR++ and does NOT include integrated RTL-SDR support. Use SDR++ or another SDR application to handle radio tuning and demodulation, then stream the audio to this app via TCP.

## Features

- TCP audio input from SDR++ or compatible applications
- Real-time signal decoding
- Support for multiple protocols (POCSAG, FLEX, DTMF, ZVEI, EAS, etc.)
- Simple UI for message display
- Audio processing via multimon-ng

## Building

```bash
cd multimon_android
flutter pub get
flutter build apk
```

## Usage

1. Configure SDR++ to output audio via TCP
2. Launch the app and connect to the TCP audio stream
3. View decoded messages in the list

See full documentation in the project for more details.
