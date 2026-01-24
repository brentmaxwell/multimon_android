package com.multimon.multimon_android

/**
 * TCP Multimon Service - TCP client that connects to SDR++ and decodes with multimon-ng
 */
class TcpMultimonService {
    
    companion object {
        init {
            System.loadLibrary("tcp_multimon")
        }
    }
    
    /**
     * Callback interface for decoded data
     */
    interface DataCallback {
        fun onDataDecoded(data: String)
    }
    
    /**
     * Start the TCP client to connect to SDR++ on localhost:7355
     * @param callback Callback for decoded messages
     * @return 0 on success, -1 on error
     */
    external fun startClient(callback: DataCallback): Int
    
    /**
     * Stop the TCP client
     * @return 0 on success, -1 on error
     */
    external fun stopClient(): Int
    
    /**
     * Check if client is running
     * @return true if running
     */
    external fun isRunning(): Boolean
    
    /**
     * Enable additional decoder (FLEX is enabled by default)
     * @param decoderName Decoder name (e.g., "POCSAG512", "AFSK1200")
     * @return 0 on success, -1 on error
     */
    external fun enableDecoder(decoderName: String): Int
}
