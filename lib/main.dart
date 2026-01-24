import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:async';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Multimon-NG',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF2196F3),
          brightness: Brightness.light,
        ),
        useMaterial3: true,
        cardTheme: const CardThemeData(
          elevation: 2,
        ),
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF2196F3),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
        cardTheme: const CardThemeData(
          elevation: 2,
        ),
      ),
      themeMode: ThemeMode.system,
      home: const MyHomePage(),
      debugShowCheckedModeBanner: false,
    );
  }
}

class MyHomePage extends StatefulWidget {
  const MyHomePage({super.key});

  @override
  State<MyHomePage> createState() => _MyHomePageState();
}

class _MyHomePageState extends State<MyHomePage> with SingleTickerProviderStateMixin {
  static const platform = MethodChannel('com.multimon.multimon_android/tcp_multimon');
  static const eventChannel = EventChannel('com.multimon.multimon_android/tcp_multimon_events');
  
  bool _isRunning = false;
  bool _isConnected = false;
  final List<DecodedMessage> _messages = [];
  StreamSubscription? _eventSubscription;
  int _messageCount = 0;
  late AnimationController _pulseController;
  
  // Available decoders
  final List<String> _availableDecoders = [
    'FLEX',
    'POCSAG512',
    'POCSAG1200',
    'POCSAG2400',
    'AFSK1200',
    'AFSK2400',
    'UFSK1200',
    'CLIPFSK',
    'FMSFSK',
    'FSK9600',
    'HAPN4800',
    'EAS',
    'DTMF',
    'ZVEI1',
    'ZVEI2',
    'ZVEI3',
    'DZVEI',
    'PZVEI',
    'EEA',
    'EIA',
    'CCIR',
    'MORSE',
    'X10',
  ];
  
  String _selectedDecoder = 'FLEX';
  
  @override
  void initState() {
    super.initState();
    _setupEventChannel();
    _pulseController = AnimationController(
      duration: const Duration(milliseconds: 1000),
      vsync: this,
    )..repeat(reverse: true);
  }
  
  void _setupEventChannel() {
    _eventSubscription = eventChannel.receiveBroadcastStream().listen(
      (dynamic data) {
        setState(() {
          _isConnected = true;
          final msg = DecodedMessage(
            timestamp: DateTime.now(),
            decoder: _selectedDecoder,
            content: data.toString(),
          );
          _messages.insert(0, msg);
          _messageCount++;
          if (_messages.length > 200) {
            _messages.removeRange(200, _messages.length);
          }
        });
      },
      onError: (dynamic error) {
        debugPrint('Event channel error: $error');
        setState(() {
          _isConnected = false;
        });
      },
    );
  }
  
  Future<void> _startClient() async {
    try {
      await platform.invokeMethod('startClient');
      setState(() {
        _isRunning = true;
      });
      
      if (_selectedDecoder != 'FLEX') {
        await platform.invokeMethod('enableDecoder', {'decoder': _selectedDecoder});
      }
      
      _showSnackBar('Connected to SDR++', Icons.check_circle, Colors.green);
    } on PlatformException catch (e) {
      _showSnackBar('Connection failed: ${e.message}', Icons.error, Colors.red);
    }
  }
  
  Future<void> _stopClient() async {
    try {
      await platform.invokeMethod('stopClient');
      setState(() {
        _isRunning = false;
        _isConnected = false;
      });
      _showSnackBar('Disconnected', Icons.info, Colors.orange);
    } on PlatformException catch (e) {
      _showSnackBar('Stop failed: ${e.message}', Icons.error, Colors.red);
    }
  }
  
  void _clearMessages() {
    setState(() {
      _messages.clear();
      _messageCount = 0;
    });
  }
  
  void _showSnackBar(String message, IconData icon, Color color) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Row(
          children: [
            Icon(icon, color: Colors.white),
            const SizedBox(width: 12),
            Expanded(child: Text(message)),
          ],
        ),
        backgroundColor: color,
        behavior: SnackBarBehavior.floating,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        duration: const Duration(seconds: 2),
      ),
    );
  }
  
  @override
  void dispose() {
    _eventSubscription?.cancel();
    _pulseController.dispose();
    super.dispose();
  }
  
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    
    return Scaffold(
      appBar: AppBar(
        title: const Row(
          children: [
            Icon(Icons.radio, size: 24),
            SizedBox(width: 12),
            Text('Multimon-NG', style: TextStyle(fontWeight: FontWeight.w600)),
          ],
        ),
        actions: [
          if (_messages.isNotEmpty)
            IconButton(
              icon: const Icon(Icons.delete_sweep),
              onPressed: _clearMessages,
              tooltip: 'Clear messages',
            ),
        ],
      ),
      body: Column(
        children: [
          // Status Card
          Container(
            margin: const EdgeInsets.all(16),
            padding: const EdgeInsets.all(20),
            decoration: BoxDecoration(
              gradient: LinearGradient(
                colors: _isRunning && _isConnected
                    ? [Colors.green.shade400, Colors.green.shade600]
                    : [Colors.grey.shade300, Colors.grey.shade400],
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
              ),
              borderRadius: BorderRadius.circular(16),
              boxShadow: [
                BoxShadow(
                  color: Colors.black.withOpacity(0.1),
                  blurRadius: 8,
                  offset: const Offset(0, 4),
                ),
              ],
            ),
            child: Column(
              children: [
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            AnimatedBuilder(
                              animation: _pulseController,
                              builder: (context, child) {
                                return Container(
                                  width: 12,
                                  height: 12,
                                  decoration: BoxDecoration(
                                    shape: BoxShape.circle,
                                    color: _isRunning && _isConnected
                                        ? Colors.white
                                        : Colors.white.withOpacity(0.5),
                                    boxShadow: _isRunning && _isConnected
                                        ? [
                                            BoxShadow(
                                              color: Colors.white.withOpacity(_pulseController.value * 0.5),
                                              blurRadius: 8,
                                              spreadRadius: 2,
                                            ),
                                          ]
                                        : null,
                                  ),
                                );
                              },
                            ),
                            const SizedBox(width: 8),
                            Text(
                              _isRunning && _isConnected
                                  ? 'CONNECTED'
                                  : _isRunning
                                      ? 'CONNECTING...'
                                      : 'DISCONNECTED',
                              style: const TextStyle(
                                color: Colors.white,
                                fontSize: 18,
                                fontWeight: FontWeight.bold,
                                letterSpacing: 1.2,
                              ),
                            ),
                          ],
                        ),
                        const SizedBox(height: 8),
                        Text(
                          'localhost:7355',
                          style: TextStyle(
                            color: Colors.white.withOpacity(0.9),
                            fontSize: 14,
                          ),
                        ),
                      ],
                    ),
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                      decoration: BoxDecoration(
                        color: Colors.white.withOpacity(0.2),
                        borderRadius: BorderRadius.circular(20),
                      ),
                      child: Text(
                        '$_messageCount',
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 24,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          
          // Controls Card
          Card(
            margin: const EdgeInsets.symmetric(horizontal: 16),
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    'Decoder',
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(height: 12),
                  DropdownButtonFormField<String>(
                    initialValue: _selectedDecoder,
                    decoration: InputDecoration(
                      border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(8),
                      ),
                      contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                      prefixIcon: const Icon(Icons.settings_input_antenna),
                    ),
                    isExpanded: true,
                    items: _availableDecoders.map((decoder) {
                      return DropdownMenuItem<String>(
                        value: decoder,
                        child: Text(decoder, style: const TextStyle(fontSize: 16)),
                      );
                    }).toList(),
                    onChanged: _isRunning ? null : (String? newValue) {
                      if (newValue != null) {
                        setState(() {
                          _selectedDecoder = newValue;
                        });
                      }
                    },
                  ),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton.icon(
                          onPressed: _isRunning ? null : _startClient,
                          icon: const Icon(Icons.play_arrow),
                          label: const Text('START'),
                          style: FilledButton.styleFrom(
                            padding: const EdgeInsets.symmetric(vertical: 16),
                            backgroundColor: Colors.green,
                          ),
                        ),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: FilledButton.icon(
                          onPressed: _isRunning ? _stopClient : null,
                          icon: const Icon(Icons.stop),
                          label: const Text('STOP'),
                          style: FilledButton.styleFrom(
                            padding: const EdgeInsets.symmetric(vertical: 16),
                            backgroundColor: Colors.red,
                          ),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          
          const SizedBox(height: 16),
          
          // Messages Header
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  'Decoded Messages',
                  style: theme.textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
                ),
                if (_messages.isNotEmpty)
                  Text(
                    '${_messages.length} messages',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
              ],
            ),
          ),
          
          const SizedBox(height: 8),
          
          // Messages List
          Expanded(
            child: _messages.isEmpty
                ? Center(
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: [
                        Icon(
                          _isRunning ? Icons.pending : Icons.radio_button_unchecked,
                          size: 64,
                          color: theme.colorScheme.onSurfaceVariant.withOpacity(0.3),
                        ),
                        const SizedBox(height: 16),
                        Text(
                          _isRunning
                              ? 'Waiting for messages...'
                              : 'Start client to begin decoding',
                          style: theme.textTheme.bodyLarge?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                          ),
                        ),
                        if (!_isRunning) ...[
                          const SizedBox(height: 8),
                          Text(
                            'Make sure SDR++ is running with\nTCP audio server on port 7355',
                            textAlign: TextAlign.center,
                            style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant.withOpacity(0.7),
                            ),
                          ),
                        ],
                      ],
                    ),
                  )
                : ListView.builder(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    itemCount: _messages.length,
                    itemBuilder: (context, index) {
                      final message = _messages[index];
                      return MessageCard(message: message);
                    },
                  ),
          ),
        ],
      ),
    );
  }
}

class DecodedMessage {
  final DateTime timestamp;
  final String decoder;
  final String content;

  DecodedMessage({
    required this.timestamp,
    required this.decoder,
    required this.content,
  });
}

class MessageCard extends StatelessWidget {
  final DecodedMessage message;

  const MessageCard({super.key, required this.message});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final timeStr = '${message.timestamp.hour.toString().padLeft(2, '0')}:'
        '${message.timestamp.minute.toString().padLeft(2, '0')}:'
        '${message.timestamp.second.toString().padLeft(2, '0')}';
    
    return Card(
      margin: const EdgeInsets.only(bottom: 8),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onLongPress: () {
          Clipboard.setData(ClipboardData(text: message.content));
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Message copied to clipboard'),
              duration: Duration(seconds: 1),
            ),
          );
        },
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    decoration: BoxDecoration(
                      color: theme.colorScheme.primaryContainer,
                      borderRadius: BorderRadius.circular(4),
                    ),
                    child: Text(
                      message.decoder,
                      style: TextStyle(
                        color: theme.colorScheme.onPrimaryContainer,
                        fontSize: 11,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    timeStr,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                      fontFamily: 'monospace',
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              SelectableText(
                message.content,
                style: const TextStyle(
                  fontFamily: 'monospace',
                  fontSize: 13,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
