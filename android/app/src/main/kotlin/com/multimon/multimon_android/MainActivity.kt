package com.multimon.multimon_android

import android.util.Log
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.multimon.multimon_android/rtlsdr"
    private val EVENT_CHANNEL = "com.multimon.multimon_android/rtlsdr_events"
    private val TAG = "MainActivity"
    
    private lateinit var rtlSdrService: RtlSdrService
    private var eventSink: EventChannel.EventSink? = null
    
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        
        rtlSdrService = RtlSdrService(applicationContext)
        
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            rtlSdrService.handleMethodCall(call, result)
        }
        
        EventChannel(flutterEngine.dartExecutor.binaryMessenger, EVENT_CHANNEL).setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    eventSink = events
                    rtlSdrService.setDataCallback { data ->
                        runOnUiThread {
                            eventSink?.success(data)
                        }
                    }
                }
                
                override fun onCancel(arguments: Any?) {
                    eventSink = null
                }
            }
        )
    }
}

