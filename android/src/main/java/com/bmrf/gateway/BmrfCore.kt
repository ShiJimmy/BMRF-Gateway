package com.bmrf.gateway

/**
 * Thin wrapper over the native BMRF core (libbmrf.so).
 *
 * The UI does not call into the parser directly: it calls [poll] on a background
 * thread, which advances the BrandMeister client, expires calls and returns a
 * JSON snapshot that is parsed by [SnapshotParser].
 */
class BmrfCore {

    private var handle: Long = 0L

    init {
        System.loadLibrary("bmrf")
        handle = nativeCreate()
    }

    fun configure(
        host: String,
        port: Int,
        repeaterId: Int,
        callsign: String,
        password: String
    ) {
        nativeConfigure(handle, host, port, repeaterId, callsign, password)
    }

    fun start() = nativeStart(handle)

    /** Start the RF receive chain (voice pipeline + PHY) without BM login. */
    fun startRf() = nativeStartRf(handle)

    fun stop() = nativeStop(handle)

    fun setAudioEnabled(enabled: Boolean) = nativeSetAudioEnabled(handle, enabled)

    /** Comma-separated talkgroups to allow for downlink playback (blank = all). */
    fun setAudioTalkgroups(csv: String) = nativeSetAudioTalkgroups(handle, csv)

    /** Independent toggles: play decoded RF voice locally / forward RF to BM. */
    fun setRfAudioEnabled(enabled: Boolean) = nativeSetRfAudioEnabled(handle, enabled)
    fun setRfForwardEnabled(enabled: Boolean) = nativeSetRfForwardEnabled(handle, enabled)

    /** DMR timeslot to play/forward (0 = both, 1 or 2). */
    fun setRfTimeslot(slot: Int) = nativeSetRfTimeslot(handle, slot)

    /** Capture-only mode: record raw IQ without demodulation (high rates). */
    fun setCaptureOnly(captureOnly: Boolean) = nativeSetCaptureOnly(handle, captureOnly)

    fun poll(nowMs: Long): String = nativePoll(handle, nowMs)

    /** Feed a raw Homebrew datagram (packet capture replay, design doc §21). */
    fun inject(datagram: ByteArray, nowMs: Long): String = nativeInject(handle, datagram, nowMs)

    /** Hand the RTL-SDR USB file descriptor to librtlsdr (call before [sdrOpen]). */
    fun setUsbFd(fd: Int) = nativeSetUsbFd(handle, fd)

    fun sdrOpen(centerHz: Int, sampleRateHz: Int, gainTenthsDb: Int, ppm: Int): Boolean =
        nativeSdrOpen(handle, centerHz, sampleRateHz, gainTenthsDb, ppm)

    fun sdrClose() = nativeSdrClose(handle)

    fun sdrRecord(path: String): Boolean = nativeSdrRecord(handle, path)
    fun sdrStopRecord() = nativeSdrStopRecord(handle)

    /** Layered capture (design doc §21): IQ/frame/BM-tx/BM-rx records. */
    fun captureStart(path: String): Boolean = nativeCaptureStart(handle, path)
    fun captureStop() = nativeCaptureStop(handle)

    fun version(): String = nativeVersion()

    fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0L
        }
    }

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeConfigure(
        handle: Long,
        host: String,
        port: Int,
        repeaterId: Int,
        callsign: String,
        password: String
    )
    private external fun nativeStart(handle: Long)
    private external fun nativeStartRf(handle: Long)
    private external fun nativeStop(handle: Long)
    private external fun nativeSetAudioEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetAudioTalkgroups(handle: Long, csv: String)
    private external fun nativeSetRfAudioEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetRfForwardEnabled(handle: Long, enabled: Boolean)
    private external fun nativeSetRfTimeslot(handle: Long, slot: Int)
    private external fun nativeSetCaptureOnly(handle: Long, captureOnly: Boolean)
    private external fun nativeSetUsbFd(handle: Long, fd: Int)
    private external fun nativeSdrOpen(
        handle: Long,
        centerHz: Int,
        sampleRateHz: Int,
        gainTenthsDb: Int,
        ppm: Int
    ): Boolean
    private external fun nativeSdrClose(handle: Long)
    private external fun nativeSdrRecord(handle: Long, path: String): Boolean
    private external fun nativeSdrStopRecord(handle: Long)
    private external fun nativeCaptureStart(handle: Long, path: String): Boolean
    private external fun nativeCaptureStop(handle: Long)
    private external fun nativePoll(handle: Long, nowMs: Long): String
    private external fun nativeInject(handle: Long, datagram: ByteArray, nowMs: Long): String
    private external fun nativeVersion(): String
}
