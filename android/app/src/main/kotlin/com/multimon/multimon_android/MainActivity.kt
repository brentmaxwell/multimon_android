package com.multimon.multimon_android

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.multimon.multimon_android/rtlsdr"
    private val EVENT_CHANNEL = "com.multimon.multimon_android/rtlsdr_events"
    private val ACTION_USB_PERMISSION = "com.multimon.multimon_android.USB_PERMISSION"
    private val TAG = "MainActivity"
    
    private lateinit var rtlSdrService: RtlSdrService
    private var eventSink: EventChannel.EventSink? = null
    private lateinit var usbManager: UsbManager
    
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (ACTION_USB_PERMISSION == intent.action) {
                synchronized(this) {
                    val device: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    }
                    
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        device?.apply {
                            Log.d(TAG, "USB permission granted for device: ${deviceName}")
                        }
                    } else {
                        Log.d(TAG, "USB permission denied for device: ${device?.deviceName}")
                    }
                }
            }
        }
    }
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }
        
        // Request permission for any connected RTL-SDR devices
        requestUsbPermissions()
    }
    
    override fun onDestroy() {
        super.onDestroy()
        try {
            unregisterReceiver(usbReceiver)
        } catch (e: Exception) {
            Log.e(TAG, "Error unregistering receiver", e)
        }
    }
    
    private fun requestUsbPermissions() {
        val deviceList = usbManager.deviceList
        Log.d(TAG, "Found ${deviceList.size} USB devices")
        
        deviceList.values.forEach { device ->
            Log.d(TAG, "USB Device: VID=${String.format("0x%04x", device.vendorId)}, PID=${String.format("0x%04x", device.productId)}")
            
            // Check if it's an RTL-SDR device
            if (isRtlSdrDevice(device)) {
                Log.d(TAG, "Found RTL-SDR device: ${device.deviceName}")
                
                if (!usbManager.hasPermission(device)) {
                    Log.d(TAG, "Requesting permission for device: ${device.deviceName}")
                    val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        PendingIntent.FLAG_MUTABLE
                    } else {
                        0
                    }
                    val permissionIntent = PendingIntent.getBroadcast(
                        this,
                        0,
                        Intent(ACTION_USB_PERMISSION),
                        flags
                    )
                    usbManager.requestPermission(device, permissionIntent)
                } else {
                    Log.d(TAG, "Already have permission for device: ${device.deviceName}")
                }
            }
        }
    }
    
    private fun isRtlSdrDevice(device: UsbDevice): Boolean {
        // Common RTL-SDR vendor/product IDs
        val rtlSdrIds = listOf(
            Pair(0x0bda, 0x2832),
            Pair(0x0bda, 0x2838),
            Pair(0x0413, 0x6001),
            Pair(0x0413, 0x6010)
        )
        return rtlSdrIds.any { it.first == device.vendorId && it.second == device.productId }
    }
    
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

