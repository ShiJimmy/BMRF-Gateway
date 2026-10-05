package com.bmrf.gateway

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File

/**
 * Long-lived foreground service hosting the native BMRF core (design doc §25).
 *
 * Also owns the RTL-SDR USB lifecycle: it requests Android USB permission, opens
 * the device and hands the file descriptor to librtlsdr (rtlsdr_set_android_fd).
 */
class BmrfService : Service() {

    private lateinit var core: BmrfCore
    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    private var pollJob: Job? = null

    private var usbConnection: UsbDeviceConnection? = null
    private var usbReceiver: BroadcastReceiver? = null
    private var sdrConfig: SdrConfig? = null
    private var configured = false
    private var recordIq = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        core = BmrfCore()
        createNotificationChannel()
        registerUsbReceiver()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, buildNotification("CONNECTING"))

        if (intent?.action == ACTION_CLOSE_SDR) {
            core.sdrStopRecord()
            core.sdrClose()
            usbConnection?.close()
            usbConnection = null
            sdrMessage.value = "RTL-SDR stopped"
            return START_STICKY
        }

        if (intent?.action == ACTION_OPEN_SDR) {
            if (!configured) {
                core.configure("4602.master.brandmeister.network", 62031, 0, "BMRF", "")
                configured = true
            }
            sdrConfig = SdrConfig(
                centerHz = intent.getIntExtra(EXTRA_SDR_CENTER, 432750000)
                    .takeIf { it > 0 } ?: 432750000,
                rateHz = intent.getIntExtra(EXTRA_SDR_RATE, 240000)
                    .takeIf { it in 100000..3200000 } ?: 240000,
                gainTenthsDb = intent.getIntExtra(EXTRA_SDR_GAIN, 0),
                ppm = intent.getIntExtra(EXTRA_SDR_PPM, 0)
            )
            recordIq = intent.getBooleanExtra(EXTRA_RECORD_IQ, false)
            if (intent.getBooleanExtra(EXTRA_CAPTURE_ONLY, false))
                core.setCaptureOnly(true)
            requestUsbPermission()
            startPolling()
            return START_STICKY
        }

        val host = intent?.getStringExtra(EXTRA_HOST) ?: "4602.master.brandmeister.network"
        val port = intent?.getIntExtra(EXTRA_PORT, 62031) ?: 62031
        val repeaterId = intent?.getIntExtra(EXTRA_REPEATER_ID, 0) ?: 0
        val callsign = intent?.getStringExtra(EXTRA_CALLSIGN) ?: "BMRF"
        val password = intent?.getStringExtra(EXTRA_PASSWORD) ?: ""

        core.configure(host, port, repeaterId, callsign, password)
        configured = true
        core.start()
        core.setAudioEnabled(audioEnabled.value)
        startPolling()
        return START_STICKY
    }

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = scope.launch {
            // Null sentinels so every setting is pushed to the core on the first
            // poll (otherwise a value set before Start would never be applied).
            var lastAudioEnabled: Boolean? = null
            var lastAudioTalkgroups: String? = null
            var lastRfAudio: Boolean? = null
            var lastRfForward: Boolean? = null
            var lastRfTimeslot: Int? = null
            while (isActive) {
                val snapshot = try {
                    SnapshotParser.parse(core.poll(System.currentTimeMillis()))
                } catch (error: Throwable) {
                    null
                }
                snapshot?.let(::applySnapshot)

                if (audioEnabled.value != lastAudioEnabled) {
                    lastAudioEnabled = audioEnabled.value
                    core.setAudioEnabled(lastAudioEnabled)
                }
                if (audioTalkgroups.value != lastAudioTalkgroups) {
                    lastAudioTalkgroups = audioTalkgroups.value
                    core.setAudioTalkgroups(lastAudioTalkgroups)
                }
                if (rfAudioEnabled.value != lastRfAudio) {
                    lastRfAudio = rfAudioEnabled.value
                    core.setRfAudioEnabled(lastRfAudio)
                }
                if (rfForwardEnabled.value != lastRfForward) {
                    lastRfForward = rfForwardEnabled.value
                    core.setRfForwardEnabled(lastRfForward)
                }
                if (rfTimeslot.value != lastRfTimeslot) {
                    lastRfTimeslot = rfTimeslot.value
                    core.setRfTimeslot(lastRfTimeslot)
                }
                delay(POLL_INTERVAL_MS)
            }
        }
    }

    // BM downlink calls whose destination talkgroup is not in the listen list are
    // hidden from the UI. RF-received calls are never filtered. A blank list shows
    // every BM call.
    private fun bmCallHidden(call: DmrCall): Boolean {
        if (!call.source.equals("BrandMeister", ignoreCase = true))
            return false
        val filter = audioTalkgroups.value.split(',', ' ', ';')
            .mapNotNull { it.trim().toLongOrNull() }
            .filter { it > 0 }
        if (filter.isEmpty())
            return false
        return call.destId !in filter
    }

    private fun applySnapshot(snapshot: CoreSnapshot) {
        snapshot.status?.let { status.value = it }
        for (event in snapshot.events) {
            when (event.type) {
                "CallStarted", "CallUpdated", "VoiceFrameReceived", "FrameReceived" ->
                    event.call?.let { if (!bmCallHidden(it)) currentCall.value = it }
                "CallEnded" -> {
                    val finished = event.call ?: continue
                    if (bmCallHidden(finished)) continue
                    if (currentCall.value?.callId == finished.callId)
                        currentCall.value = null
                    val entry = CallHistoryEntry(finished, System.currentTimeMillis())
                    history.value = (listOf(entry) + history.value).take(MAX_HISTORY)
                }
            }
        }
    }

    // ---- RTL-SDR USB -------------------------------------------------------

    private fun registerUsbReceiver() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent?.action != ACTION_USB_PERM)
                    return
                val device: UsbDevice? =
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    else
                        @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                if (granted && device != null)
                    openUsb(device)
                else
                    sdrMessage.value = "USB permission denied"
            }
        }
        usbReceiver = receiver
        ContextCompat.registerReceiver(
            this, receiver, IntentFilter(ACTION_USB_PERM), ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    private fun requestUsbPermission() {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        val device = manager.deviceList.values.firstOrNull {
            it.vendorId == RTL_USB_VID && it.productId == RTL_USB_PID
        }
        if (device == null) {
            sdrMessage.value = "RTL-SDR not found (0bda:2838)"
            return
        }
        if (manager.hasPermission(device)) {
            openUsb(device)
            return
        }
        val flags = PendingIntent.FLAG_IMMUTABLE
        val intent = Intent(ACTION_USB_PERM).setPackage(packageName)
        val pending = PendingIntent.getBroadcast(this, 0, intent, flags)
        sdrMessage.value = "requesting USB permission..."
        manager.requestPermission(device, pending)
    }

    private fun openUsb(device: UsbDevice) {
        val manager = getSystemService(Context.USB_SERVICE) as UsbManager
        val connection = manager.openDevice(device)
        if (connection == null) {
            sdrMessage.value = "openDevice failed"
            return
        }
        usbConnection = connection
        val config = sdrConfig ?: SdrConfig()
        core.setUsbFd(connection.fileDescriptor)
        // librtlsdr open/retune and starting the async reader perform blocking
        // USB I/O; keep them off the main thread so the UI never stalls.
        scope.launch {
            // Keep the DMR carrier above the SDR centre so it falls outside the
            // RTL-SDR DC spike. At low sample rates the resampler rolls off well
            // before Nyquist, so a large offset would attenuate the signal; use a
            // small one that is still clear of the DC spike. The demod's DC
            // blocker removes the residual carrier.
            val dcOffsetHz = if (config.rateHz <= 300000) DC_OFFSET_HZ_LOW else DC_OFFSET_HZ
            val sdrCenterHz = config.centerHz - dcOffsetHz
            val ok = core.sdrOpen(sdrCenterHz, config.rateHz, config.gainTenthsDb, config.ppm)
            if (ok) {
                core.startRf()
                core.setRfAudioEnabled(rfAudioEnabled.value)
                core.setRfForwardEnabled(rfForwardEnabled.value)
                core.setRfTimeslot(rfTimeslot.value)
                if (recordIq) {
                    val file = File(getExternalFilesDir(null), "bmrf_iq.cu8")
                    core.sdrRecord(file.absolutePath)
                    val capture = File(getExternalFilesDir(null), "bmrf_capture.txt")
                    core.captureStart(capture.absolutePath)
                    sdrMessage.value = "recording ${file.absolutePath} + capture"
                } else {
                    sdrMessage.value = "RTL-SDR opened"
                }
            } else {
                sdrMessage.value = "RTL-SDR open failed"
            }
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (manager.getNotificationChannel(CHANNEL_ID) == null) {
                manager.createNotificationChannel(
                    NotificationChannel(CHANNEL_ID, "BMRF-Gateway", NotificationManager.IMPORTANCE_LOW)
                )
            }
        }
    }

    private fun buildNotification(state: String): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("BMRF-Gateway")
            .setContentText("BrandMeister: $state")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

    override fun onDestroy() {
        pollJob?.cancel()
        scope.cancel()
        core.captureStop()
        core.sdrClose()
        usbConnection?.close()
        usbConnection = null
        usbReceiver?.let { runCatching { unregisterReceiver(it) } }
        core.stop()
        core.close()
        // The companion StateFlows are process-wide, so clear them here or the
        // UI keeps showing the last (e.g. ACTIVE) snapshot after Stop.
        status.value = null
        currentCall.value = null
        super.onDestroy()
    }

    private data class SdrConfig(
        val centerHz: Int = 432750000,
        val rateHz: Int = 240000,
        val gainTenthsDb: Int = 0,
        val ppm: Int = 0
    )

    companion object {
        const val ACTION_OPEN_SDR = "com.bmrf.gateway.OPEN_SDR"
        const val ACTION_CLOSE_SDR = "com.bmrf.gateway.CLOSE_SDR"
        private const val ACTION_USB_PERM = "com.bmrf.gateway.USB_PERM"
        private const val RTL_USB_VID = 0x0bda
        private const val RTL_USB_PID = 0x2838
        private const val DC_OFFSET_HZ = 100000
        private const val DC_OFFSET_HZ_LOW = 40000

        const val EXTRA_HOST = "com.bmrf.gateway.HOST"
        const val EXTRA_PORT = "com.bmrf.gateway.PORT"
        const val EXTRA_REPEATER_ID = "com.bmrf.gateway.REPEATER_ID"
        const val EXTRA_CALLSIGN = "com.bmrf.gateway.CALLSIGN"
        const val EXTRA_PASSWORD = "com.bmrf.gateway.PASSWORD"
        const val EXTRA_SDR_CENTER = "com.bmrf.gateway.SDR_CENTER"
        const val EXTRA_SDR_RATE = "com.bmrf.gateway.SDR_RATE"
        const val EXTRA_SDR_GAIN = "com.bmrf.gateway.SDR_GAIN"
        const val EXTRA_SDR_PPM = "com.bmrf.gateway.SDR_PPM"
        const val EXTRA_RECORD_IQ = "com.bmrf.gateway.RECORD_IQ"
        const val EXTRA_CAPTURE_ONLY = "com.bmrf.gateway.CAPTURE_ONLY"

        val status = MutableStateFlow<NetworkStatus?>(null)
        val currentCall = MutableStateFlow<DmrCall?>(null)
        val history = MutableStateFlow<List<CallHistoryEntry>>(emptyList())
        val audioEnabled = MutableStateFlow(false)
        val audioTalkgroups = MutableStateFlow("")
        val rfAudioEnabled = MutableStateFlow(false)
        val rfForwardEnabled = MutableStateFlow(false)
        val rfTimeslot = MutableStateFlow(1)
        val sdrMessage = MutableStateFlow<String?>(null)

        private const val CHANNEL_ID = "bmrf_gateway"
        private const val NOTIFICATION_ID = 1001
        private const val POLL_INTERVAL_MS = 100L
        private const val MAX_HISTORY = 50
    }
}
