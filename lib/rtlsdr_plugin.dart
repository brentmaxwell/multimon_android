import 'dart:async';
import 'package:flutter/services.dart';

class RtlSdrPlugin {
  static const MethodChannel _channel = MethodChannel('com.multimon.multimon_android/rtlsdr');
  static const EventChannel _eventChannel = EventChannel('com.multimon.multimon_android/rtlsdr_events');
  
  static Stream<String>? _dataStream;
  
  /// Get the number of RTL-SDR devices connected
  static Future<int> getDeviceCount() async {
    try {
      final int count = await _channel.invokeMethod('getDeviceCount');
      return count;
    } catch (e) {
      print('Error getting device count: $e');
      return 0;
    }
  }
  
  /// Get the name of a specific device
  static Future<String?> getDeviceName(int index) async {
    try {
      final String? name = await _channel.invokeMethod('getDeviceName', {'index': index});
      return name;
    } catch (e) {
      print('Error getting device name: $e');
      return null;
    }
  }
  
  /// Open an RTL-SDR device
  static Future<bool> openDevice(int index) async {
    try {
      final bool result = await _channel.invokeMethod('openDevice', {'index': index});
      return result;
    } catch (e) {
      print('Error opening device: $e');
      return false;
    }
  }
  
  /// Close the currently open device
  static Future<void> closeDevice() async {
    try {
      await _channel.invokeMethod('closeDevice');
    } catch (e) {
      print('Error closing device: $e');
    }
  }
  
  /// Set the center frequency in Hz
  static Future<bool> setFrequency(int frequency) async {
    try {
      final bool result = await _channel.invokeMethod('setFrequency', {'frequency': frequency});
      return result;
    } catch (e) {
      print('Error setting frequency: $e');
      return false;
    }
  }
  
  /// Set the sample rate in Hz
  static Future<bool> setSampleRate(int rate) async {
    try {
      final bool result = await _channel.invokeMethod('setSampleRate', {'rate': rate});
      return result;
    } catch (e) {
      print('Error setting sample rate: $e');
      return false;
    }
  }
  
  /// Set the tuner gain (in tenths of dB)
  static Future<bool> setGain(int gain) async {
    try {
      final bool result = await _channel.invokeMethod('setGain', {'gain': gain});
      return result;
    } catch (e) {
      print('Error setting gain: $e');
      return false;
    }
  }
  
  /// Add a demodulator
  /// Types: POCSAG512, POCSAG1200, POCSAG2400, FLEX, EAS, DTMF, ZVEI, etc.
  static Future<int> addDemodulator(String type) async {
    try {
      final int id = await _channel.invokeMethod('addDemodulator', {'type': type});
      return id;
    } catch (e) {
      print('Error adding demodulator: $e');
      return -1;
    }
  }
  
  /// Start receiving and decoding
  static Future<void> startReceiving() async {
    try {
      await _channel.invokeMethod('startReceiving');
    } catch (e) {
      print('Error starting receiving: $e');
    }
  }
  
  /// Stop receiving
  static Future<void> stopReceiving() async {
    try {
      await _channel.invokeMethod('stopReceiving');
    } catch (e) {
      print('Error stopping receiving: $e');
    }
  }
  
  /// Get a stream of decoded data
  static Stream<String> getDataStream() {
    _dataStream ??= _eventChannel.receiveBroadcastStream().map((event) => event.toString());
    return _dataStream!;
  }
  
  // ============== RTL-TCP Methods ==============
  
  /// Connect to RTL-TCP server
  static Future<bool> connectTcp({String host = '192.168.1.240', int port = 1234}) async {
    try {
      final bool result = await _channel.invokeMethod('connectTcp', {'host': host, 'port': port});
      return result;
    } catch (e) {
      print('Error connecting to TCP: $e');
      return false;
    }
  }
  
  /// Disconnect from RTL-TCP server
  static Future<void> disconnectTcp() async {
    try {
      await _channel.invokeMethod('disconnectTcp');
    } catch (e) {
      print('Error disconnecting TCP: $e');
    }
  }
  
  /// Set TCP frequency
  static Future<bool> setTcpFrequency(int frequency) async {
    try {
      final bool result = await _channel.invokeMethod('setTcpFrequency', {'frequency': frequency});
      return result;
    } catch (e) {
      print('Error setting TCP frequency: $e');
      return false;
    }
  }
  
  /// Set TCP sample rate
  static Future<bool> setTcpSampleRate(int rate) async {
    try {
      final bool result = await _channel.invokeMethod('setTcpSampleRate', {'rate': rate});
      return result;
    } catch (e) {
      print('Error setting TCP sample rate: $e');
      return false;
    }
  }
  
  /// Set TCP gain
  static Future<bool> setTcpGain(int gain) async {
    try {
      final bool result = await _channel.invokeMethod('setTcpGain', {'gain': gain});
      return result;
    } catch (e) {
      print('Error setting TCP gain: $e');
      return false;
    }
  }
  
  /// Start TCP receiving
  static Future<void> startTcpReceiving() async {
    try {
      await _channel.invokeMethod('startTcpReceiving');
    } catch (e) {
      print('Error starting TCP receiving: $e');
    }
  }
  
  /// Check if TCP is connected
  static Future<bool> isTcpConnected() async {
    try {
      final bool result = await _channel.invokeMethod('isTcpConnected');
      return result;
    } catch (e) {
      print('Error checking TCP connection: $e');
      return false;
    }
  }
  
  /// Enable/disable audio output for debugging
  static Future<void> enableAudioOutput(bool enable) async {
    try {
      await _channel.invokeMethod('enableAudioOutput', {'enable': enable});
    } catch (e) {
      print('Error setting audio output: $e');
    }
  }
}
