package com.multimon.multimon_android

import android.os.Bundle
import android.util.Log
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

/**
 * MainActivity for TCP-based multimon-ng decoding
 * 
 * This app connects to SDR++ on localhost:7355 to receive raw audio and decodes it with multimon-ng.
 * Usage: Start SDR++ with TCP audio server on port 7355, then connect from this app
 */
class MainActivityTcp : FlutterActivity() {
    private val CHANNEL = "com.multimon.multimon_android/tcp_multimon"
    private val EVENT_CHANNEL = "com.multimon.multimon_android/tcp_multimon_events"
    private val TAG = "MainActivityTcp"
    
    private lateinit var tcpService: TcpMultimonService
    private var eventSink: EventChannel.EventSink? = null
    
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        
        tcpService = TcpMultimonService()
        
        // Method channel for controlling the TCP client
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "startClient" -> {
                    val ret = tcpService.startClient(object : TcpMultimonService.DataCallback {
                        override fun onDataDecoded(data: String) {
                            runOnUiThread {
                                eventSink?.success(data)
                            }
                        }
                    })
                    if (ret == 0) {
                        result.success("Client started, connecting to localhost:7355")
                    } else {
                        result.error("START_FAILED", "Failed to start TCP client", null)
                    }
                }
                
                "stopClient" -> {
                    val ret = tcpService.stopClient()
                    if (ret == 0) {
                        result.success("Client stopped")
                    } else {
                        result.error("STOP_FAILED", "Failed to stop TCP client", null)
                    }
                }
                
                "isRunning" -> {
                    result.success(tcpService.isRunning())
                }
                
                "enableDecoder" -> {
                    val decoderName = call.argument<String>("decoder") ?: "FLEX"
                    val ret = tcpService.enableDecoder(decoderName)
                    if (ret == 0) {
                        result.success("Decoder $decoderName enabled")
                    } else {
                        result.error("DECODER_FAILED", "Failed to enable decoder $decoderName", null)
                    }
                }
                
                else -> {
                    result.notImplemented()
                }
            }
        }
        
        // Event channel for decoded data
        EventChannel(flutterEngine.dartExecutor.binaryMessenger, EVENT_CHANNEL).setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    eventSink = events
                    Log.d(TAG, "Event channel listener attached")
                }
                
                override fun onCancel(arguments: Any?) {
                    eventSink = null
                    Log.d(TAG, "Event channel listener cancelled")
                }
            }
        )
    }
    
    override fun onDestroy() {
        super.onDestroy()
        // Stop client if running
        try {
            tcpService.stopClient()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping TCP client", e)
        }
    }
}
