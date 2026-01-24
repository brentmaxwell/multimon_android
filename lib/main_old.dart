import 'dart:async';
import 'package:flutter/material.dart';
import 'rtlsdr_plugin.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Multimon RTL-SDR',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: const MultimonHomePage(),
    );
  }
}

class MultimonHomePage extends StatefulWidget {
  const MultimonHomePage({super.key});

  @override
  State<MultimonHomePage> createState() => _MultimonHomePageState();
}

class _MultimonHomePageState extends State<MultimonHomePage> {
  int _deviceCount = 0;
  String? _deviceName;
  bool _isConnected = false;
  bool _isReceiving = false;
  bool _isTcpMode = false;  // TCP vs USB mode
  bool _audioOutputEnabled = false;  // Audio debug output
  int _frequency = 144390000; // 144.39 MHz - APRS frequency
  int _sampleRate = 220500; // 22050 * 10 for exact division to 22050 Hz
  int _gain = 400; // 40.0 dB
  final List<String> _decodedMessages = [];
  StreamSubscription<String>? _dataSubscription;
  
  // TCP settings
  final String _tcpHost = '192.168.1.240';
  final int _tcpPort = 1234;

  @override
  void initState() {
    super.initState();
    _checkDevices();
  }

  @override
  void dispose() {
    _dataSubscription?.cancel();
    if (_isConnected) {
      if (_isTcpMode) {
        RtlSdrPlugin.disconnectTcp();
      } else {
        RtlSdrPlugin.closeDevice();
      }
    }
    super.dispose();
  }

  Future<void> _checkDevices() async {
    final count = await RtlSdrPlugin.getDeviceCount();
    String? name;
    if (count > 0) {
      name = await RtlSdrPlugin.getDeviceName(0);
    }
    setState(() {
      _deviceCount = count;
      _deviceName = name;
    });
  }

  Future<void> _connectDevice() async {
    if (_deviceCount == 0) {
      _showMessage('No RTL-SDR devices found');
      return;
    }

    final success = await RtlSdrPlugin.openDevice(0);
    if (success) {
      await RtlSdrPlugin.setFrequency(_frequency);
      await RtlSdrPlugin.setSampleRate(_sampleRate);
      await RtlSdrPlugin.setGain(_gain);
      await RtlSdrPlugin.addDemodulator('AFSK1200');

      setState(() {
        _isConnected = true;
        _isTcpMode = false;
      });
      _showMessage('Connected to USB device - APRS 144.39 MHz');
    } else {
      _showMessage('Failed to connect to device');
    }
  }

  Future<void> _connectTcp() async {
    _showMessage('Connecting to $_tcpHost:$_tcpPort...');
    
    final success = await RtlSdrPlugin.connectTcp(host: _tcpHost, port: _tcpPort);
    if (success) {
      await RtlSdrPlugin.setTcpFrequency(_frequency);
      await RtlSdrPlugin.setTcpSampleRate(_sampleRate);
      await RtlSdrPlugin.setTcpGain(_gain);
      await RtlSdrPlugin.addDemodulator('AFSK1200');

      setState(() {
        _isConnected = true;
        _isTcpMode = true;
      });
      _showMessage('Connected to RTL-TCP at $_tcpHost:$_tcpPort');
    } else {
      _showMessage('Failed to connect to RTL-TCP server');
    }
  }

  Future<void> _disconnectDevice() async {
    if (_isReceiving) {
      await _stopReceiving();
    }
    
    if (_isTcpMode) {
      await RtlSdrPlugin.disconnectTcp();
    } else {
      await RtlSdrPlugin.closeDevice();
    }
    
    setState(() {
      _isConnected = false;
      _isTcpMode = false;
    });
    _showMessage('Disconnected');
  }

  Future<void> _startReceiving() async {
    if (!_isConnected) return;

    _dataSubscription = RtlSdrPlugin.getDataStream().listen((data) {
      setState(() {
        _decodedMessages.insert(0, data);
        if (_decodedMessages.length > 100) {
          _decodedMessages.removeLast();
        }
      });
    });

    if (_isTcpMode) {
      await RtlSdrPlugin.startTcpReceiving();
    } else {
      await RtlSdrPlugin.startReceiving();
    }
    
    setState(() {
      _isReceiving = true;
    });
  }

  Future<void> _stopReceiving() async {
    await RtlSdrPlugin.stopReceiving();
    await _dataSubscription?.cancel();
    _dataSubscription = null;
    if (_audioOutputEnabled) {
      await RtlSdrPlugin.enableAudioOutput(false);
      _audioOutputEnabled = false;
    }
    setState(() {
      _isReceiving = false;
    });
  }

  Future<void> _toggleAudioOutput() async {
    final newState = !_audioOutputEnabled;
    await RtlSdrPlugin.enableAudioOutput(newState);
    setState(() {
      _audioOutputEnabled = newState;
    });
    _showMessage(newState ? 'Audio output enabled' : 'Audio output disabled');
  }

  void _showMessage(String message) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message)),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        title: const Text('Multimon RTL-SDR'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16.0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'RTL-SDR Status',
                      style: Theme.of(context).textTheme.titleLarge,
                    ),
                    const SizedBox(height: 8),
                    Text('USB Devices found: $_deviceCount'),
                    if (_deviceName != null) Text('Device: $_deviceName'),
                    Text('Status: ${_isConnected ? (_isTcpMode ? "Connected (TCP)" : "Connected (USB)") : "Disconnected"}'),
                    if (_isConnected) ...[
                      Text('Frequency: ${(_frequency / 1000000).toStringAsFixed(3)} MHz'),
                      Text('Sample Rate: ${(_sampleRate / 1000).toStringAsFixed(1)} kS/s'),
                      Text('Gain: ${(_gain / 10).toStringAsFixed(1)} dB'),
                    ],
                    const SizedBox(height: 12),
                    Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      children: [
                        ElevatedButton(
                          onPressed: _isConnected ? null : _connectDevice,
                          child: const Text('USB'),
                        ),
                        ElevatedButton(
                          onPressed: _isConnected ? null : _connectTcp,
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.orange,
                          ),
                          child: const Text('TCP'),
                        ),
                        ElevatedButton(
                          onPressed: _isConnected ? _disconnectDevice : null,
                          child: const Text('Disconnect'),
                        ),
                        ElevatedButton(
                          onPressed: _isConnected && !_isReceiving ? _startReceiving : null,
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.green,
                          ),
                          child: const Text('Start'),
                        ),
                        ElevatedButton(
                          onPressed: _isReceiving ? _stopReceiving : null,
                          style: ElevatedButton.styleFrom(
                            backgroundColor: Colors.red,
                          ),
                          child: const Text('Stop'),
                        ),
                        ElevatedButton(
                          onPressed: _isReceiving ? _toggleAudioOutput : null,
                          style: ElevatedButton.styleFrom(
                            backgroundColor: _audioOutputEnabled ? Colors.purple : Colors.grey,
                          ),
                          child: Text(_audioOutputEnabled ? '🔊 Audio' : '🔇 Audio'),
                        ),
                      ],
                    ),
                    if (!_isConnected) ...[
                      const SizedBox(height: 8),
                      Text(
                        'TCP Server: $_tcpHost:$_tcpPort',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ],
                  ],
                ),
              ),
            ),
            const SizedBox(height: 16),
            Text(
              'Multimon-ng Output',
              style: Theme.of(context).textTheme.titleLarge,
            ),
            const SizedBox(height: 8),
            Expanded(
              child: Card(
                child: _decodedMessages.isEmpty
                    ? const Center(child: Text('Waiting for signals...\nListening on 144.39 MHz APRS'))
                    : ListView.builder(
                        reverse: true,
                        itemCount: _decodedMessages.length,
                        itemBuilder: (context, index) {
                          return Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                            child: Text(
                              _decodedMessages[index],
                              style: const TextStyle(
                                fontFamily: 'monospace',
                                fontSize: 11,
                                height: 1.3,
                              ),
                            ),
                          );
                        },
                      ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
