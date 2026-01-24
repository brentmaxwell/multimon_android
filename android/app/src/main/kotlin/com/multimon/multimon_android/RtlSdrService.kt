package com.multimon.multimon_android

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

class RtlSdrService(private val context: Context) {
    
    companion object {
        private const val TAG = "RtlSdrService"
        private const val ACTION_USB_PERMISSION = "com.multimon.multimon_android.USB_PERMISSION"
        
        init {
            System.loadLibrary("multimon_rtlsdr")
        }
    }
    
    private var callback: ((String) -> Unit)? = null
    private val usbManager: UsbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
    
    // Native methods - USB
    external fun getDeviceCount(): Int
    external fun getDeviceName(index: Int): String?
    external fun openDevice(index: Int): Int
    external fun openDeviceWithFd(fd: Int, path: String): Int
    external fun closeDevice()
    external fun setFrequency(freq: Int): Int
    external fun setSampleRate(rate: Int): Int
    external fun setGain(gain: Int): Int
    external fun addDemodulator(type: String): Int
    external fun startReceiving(callback: DataCallback)
    external fun stopReceiving()
    
    // Native methods - TCP
    external fun connectTcp(host: String, port: Int): Int
    external fun disconnectTcp()
    external fun setTcpFrequency(freq: Int): Int
    external fun setTcpSampleRate(rate: Int): Int
    external fun setTcpGain(gain: Int): Int
    external fun startTcpReceiving(callback: DataCallback)
    external fun isTcpConnected(): Boolean
    
    // Native methods - Audio output for debugging
    external fun enableAudioOutput(enable: Boolean)
    external fun getAudioSamples(buffer: ShortArray): Int
    
    private fun getRtlSdrDevice(index: Int): UsbDevice? {
        val devices = usbManager.deviceList.values.filter { isRtlSdrDevice(it) }
        return if (index < devices.size) devices.toList()[index] else null
    }
    
    private fun requestUsbPermissionsSync() {
        val deviceList = usbManager.deviceList
        Log.d(TAG, "Checking ${deviceList.size} USB devices")
        
        deviceList.values.forEach { device ->
            Log.d(TAG, "USB Device: VID=${String.format("0x%04x", device.vendorId)}, PID=${String.format("0x%04x", device.productId)}")
            
            if (isRtlSdrDevice(device)) {
                Log.d(TAG, "Found RTL-SDR device!")
                if (!usbManager.hasPermission(device)) {
                    Log.d(TAG, "No permission yet, requesting...")
                } else {
                    Log.d(TAG, "Already have permission")
                }
            }
        }
    }
    
    private fun isRtlSdrDevice(device: UsbDevice): Boolean {
        val rtlSdrIds = listOf(
            Pair(0x0bda, 0x2832),
            Pair(0x0bda, 0x2838),
            Pair(0x0413, 0x6001),
            Pair(0x0413, 0x6010)
        )
        return rtlSdrIds.any { it.first == device.vendorId && it.second == device.productId }
    }
    
    // Callback interface for native code
    interface DataCallback {
        fun onDataDecoded(data: String)
    }
    
    fun handleMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "getDeviceCount" -> {
                try {
                    // Count RTL-SDR devices from Android USB manager
                    val devices = usbManager.deviceList.values.filter { isRtlSdrDevice(it) }
                    val count = devices.size
                    
                    Log.d(TAG, "Found $count RTL-SDR device(s)")
                    devices.forEachIndexed { index, device ->
                        val hasPermission = usbManager.hasPermission(device)
                        Log.d(TAG, "Device $index: VID=${String.format("0x%04x", device.vendorId)}, " +
                                "PID=${String.format("0x%04x", device.productId)}, " +
                                "Permission=$hasPermission")
                    }
                    
                    result.success(count)
                } catch (e: Exception) {
                    Log.e(TAG, "Error getting device count", e)
                    result.error("ERROR", e.message, null)
                }
            }
            "getDeviceName" -> {
                try {
                    val index = call.argument<Int>("index") ?: 0
                    val device = getRtlSdrDevice(index)
                    val name = if (device != null) {
                        "${device.manufacturerName ?: "RTL-SDR"} ${device.productName ?: "Device"}"
                    } else {
                        null
                    }
                    Log.d(TAG, "Device $index name: $name")
                    result.success(name)
                } catch (e: Exception) {
                    Log.e(TAG, "Error getting device name", e)
                    result.error("ERROR", e.message, null)
                }
            }
            "openDevice" -> {
                try {
                    val index = call.argument<Int>("index") ?: 0
                    
                    // Use Android USB manager to get file descriptor
                    val device = getRtlSdrDevice(index)
                    if (device == null) {
                        Log.e(TAG, "No RTL-SDR device found at index $index")
                        result.error("NO_DEVICE", "No RTL-SDR device found", null)
                        return
                    }
                    
                    if (!usbManager.hasPermission(device)) {
                        Log.e(TAG, "No permission for device ${device.deviceName}")
                        result.error("NO_PERMISSION", "No USB permission", null)
                        return
                    }
                    
                    val connection = usbManager.openDevice(device)
                    if (connection == null) {
                        Log.e(TAG, "Failed to open USB connection")
                        result.error("OPEN_FAILED", "Failed to open USB connection", null)
                        return
                    }
                    
                    val fd = connection.fileDescriptor
                    val path = device.deviceName
                    
                    Log.d(TAG, "Opening device with FD=$fd, path=$path")
                    val ret = openDeviceWithFd(fd, path)
                    
                    if (ret == 0) {
                        Log.d(TAG, "Device opened successfully")
                        result.success(true)
                    } else {
                        connection.close()
                        Log.e(TAG, "Failed to open device, error code: $ret")
                        result.error("OPEN_FAILED", "Failed to open device", ret)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error opening device", e)
                    result.error("ERROR", e.message, null)
                }
            }
            "closeDevice" -> {
                try {
                    closeDevice()
                    result.success(null)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setFrequency" -> {
                try {
                    val freq = call.argument<Int>("frequency") ?: 0
                    val ret = setFrequency(freq)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setSampleRate" -> {
                try {
                    val rate = call.argument<Int>("rate") ?: 2048000
                    val ret = setSampleRate(rate)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setGain" -> {
                try {
                    val gain = call.argument<Int>("gain") ?: 0
                    val ret = setGain(gain)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "addDemodulator" -> {
                try {
                    val type = call.argument<String>("type") ?: "POCSAG512"
                    val id = addDemodulator(type)
                    result.success(id)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "startReceiving" -> {
                try {
                    val dataCallback = object : DataCallback {
                        override fun onDataDecoded(data: String) {
                            Log.d(TAG, "Decoded: $data")
                            callback?.invoke(data)
                        }
                    }
                    startReceiving(dataCallback)
                    result.success(null)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "stopReceiving" -> {
                try {
                    stopReceiving()
                    result.success(null)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            // RTL-TCP methods
            "connectTcp" -> {
                try {
                    val host = call.argument<String>("host") ?: "192.168.1.240"
                    val port = call.argument<Int>("port") ?: 1234
                    Log.d(TAG, "Connecting to RTL-TCP at $host:$port")
                    val ret = connectTcp(host, port)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    Log.e(TAG, "Error connecting to TCP", e)
                    result.error("ERROR", e.message, null)
                }
            }
            "disconnectTcp" -> {
                try {
                    disconnectTcp()
                    result.success(null)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setTcpFrequency" -> {
                try {
                    val freq = call.argument<Int>("frequency") ?: 0
                    val ret = setTcpFrequency(freq)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setTcpSampleRate" -> {
                try {
                    val rate = call.argument<Int>("rate") ?: 240000
                    val ret = setTcpSampleRate(rate)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "setTcpGain" -> {
                try {
                    val gain = call.argument<Int>("gain") ?: 0
                    val ret = setTcpGain(gain)
                    result.success(ret == 0)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "startTcpReceiving" -> {
                try {
                    val dataCallback = object : DataCallback {
                        override fun onDataDecoded(data: String) {
                            Log.d(TAG, "TCP Decoded: $data")
                            callback?.invoke(data)
                        }
                    }
                    startTcpReceiving(dataCallback)
                    result.success(null)
                } catch (e: Exception) {
                    Log.e(TAG, "Error starting TCP receiving", e)
                    result.error("ERROR", e.message, null)
                }
            }
            "isTcpConnected" -> {
                try {
                    result.success(isTcpConnected())
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            "enableAudioOutput" -> {
                try {
                    val enable = call.argument<Boolean>("enable") ?: false
                    enableAudioOutput(enable)
                    if (enable) {
                        startAudioPlayback()
                    } else {
                        stopAudioPlayback()
                    }
                    result.success(null)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }
            else -> result.notImplemented()
        }
    }
    
    // Audio playback
    private var audioTrack: android.media.AudioTrack? = null
    private var audioThread: Thread? = null
    private var audioPlaying = false
    
    private fun startAudioPlayback() {
        if (audioPlaying) return
        
        val sampleRate = 22050
        val bufferSize = android.media.AudioTrack.getMinBufferSize(
            sampleRate,
            android.media.AudioFormat.CHANNEL_OUT_MONO,
            android.media.AudioFormat.ENCODING_PCM_16BIT
        )
        
        audioTrack = android.media.AudioTrack(
            android.media.AudioManager.STREAM_MUSIC,
            sampleRate,
            android.media.AudioFormat.CHANNEL_OUT_MONO,
            android.media.AudioFormat.ENCODING_PCM_16BIT,
            bufferSize * 4,
            android.media.AudioTrack.MODE_STREAM
        )
        
        audioPlaying = true
        audioTrack?.play()
        
        audioThread = Thread {
            val buffer = ShortArray(2048)
            while (audioPlaying) {
                val samples = getAudioSamples(buffer)
                if (samples > 0) {
                    audioTrack?.write(buffer, 0, samples)
                } else {
                    Thread.sleep(10)
                }
            }
        }
        audioThread?.start()
        Log.d(TAG, "Audio playback started")
    }
    
    private fun stopAudioPlayback() {
        audioPlaying = false
        audioThread?.join(1000)
        audioThread = null
        audioTrack?.stop()
        audioTrack?.release()
        audioTrack = null
        Log.d(TAG, "Audio playback stopped")
    }
    
    fun setDataCallback(callback: (String) -> Unit) {
        this.callback = callback
    }
}
