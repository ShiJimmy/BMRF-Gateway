package com.bmrf.gateway

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.switchmaterial.SwitchMaterial
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale

class MainActivity : AppCompatActivity() {

    private lateinit var hostInput: EditText
    private lateinit var portInput: EditText
    private lateinit var idInput: EditText
    private lateinit var callsignInput: EditText
    private lateinit var passwordInput: EditText
    private lateinit var apiKeyInput: EditText
    private lateinit var ts1Input: EditText
    private lateinit var ts2Input: EditText
    private lateinit var audioTalkgroupsInput: EditText

    private lateinit var prefs: SharedPreferences

    private lateinit var stateChip: TextView
    private lateinit var callTitleText: TextView
    private lateinit var callDetailText: TextView
    private lateinit var audioStatusText: TextView
    private lateinit var decoderText: TextView
    private lateinit var networkDetailText: TextView
    private lateinit var statsText: TextView
    private lateinit var audioSwitch: SwitchMaterial
    private lateinit var sdrFreqInput: EditText
    private lateinit var sdrRateInput: EditText
    private lateinit var sdrGainInput: EditText
    private lateinit var sdrPpmInput: EditText
    private lateinit var sdrStatusText: TextView
    private lateinit var rfTimeslotInput: EditText
    private lateinit var rfReceiveSwitch: SwitchMaterial
    private lateinit var rfAudioSwitch: SwitchMaterial
    private lateinit var rfForwardSwitch: SwitchMaterial
    private lateinit var contactStatusText: TextView
    private lateinit var contactsPicker: ActivityResultLauncher<Array<String>>
    private var suppressSwitchCallback = false

    private val adapter = CallAdapter()

    private var recordIq = false
    private var captureOnly = false
    private var launchFreqHz = 0
    private var launchRateHz = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        prefs = getSharedPreferences("bmrf_conn", MODE_PRIVATE)

        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        idInput = findViewById(R.id.idInput)
        callsignInput = findViewById(R.id.callsignInput)
        passwordInput = findViewById(R.id.passwordInput)
        apiKeyInput = findViewById(R.id.apiKeyInput)
        ts1Input = findViewById(R.id.ts1Input)
        ts2Input = findViewById(R.id.ts2Input)
        audioTalkgroupsInput = findViewById(R.id.audioTalkgroupsInput)

        stateChip = findViewById(R.id.networkStateChip)
        callTitleText = findViewById(R.id.callTitleText)
        callDetailText = findViewById(R.id.callDetailText)
        audioStatusText = findViewById(R.id.audioStatusText)
        decoderText = findViewById(R.id.decoderText)
        networkDetailText = findViewById(R.id.networkDetailText)
        statsText = findViewById(R.id.statsText)
        audioSwitch = findViewById(R.id.audioSwitch)
        sdrFreqInput = findViewById(R.id.sdrFreqInput)
        sdrRateInput = findViewById(R.id.sdrRateInput)
        sdrGainInput = findViewById(R.id.sdrGainInput)
        sdrPpmInput = findViewById(R.id.sdrPpmInput)
        sdrStatusText = findViewById(R.id.sdrStatusText)
        rfTimeslotInput = findViewById(R.id.rfTimeslotInput)
        rfReceiveSwitch = findViewById(R.id.rfReceiveSwitch)
        rfAudioSwitch = findViewById(R.id.rfAudioSwitch)
        rfForwardSwitch = findViewById(R.id.rfForwardSwitch)
        contactStatusText = findViewById(R.id.contactStatusText)

        findViewById<RecyclerView>(R.id.historyList).apply {
            layoutManager = LinearLayoutManager(this@MainActivity)
            adapter = this@MainActivity.adapter
        }

        audioSwitch.isChecked = BmrfService.audioEnabled.value
        audioSwitch.setOnCheckedChangeListener { _, isChecked ->
            BmrfService.audioEnabled.value = isChecked
        }

        audioTalkgroupsInput.doAfterTextChanged {
            BmrfService.audioTalkgroups.value = it?.toString()?.trim() ?: ""
        }

        rfReceiveSwitch.setOnCheckedChangeListener { _, isChecked ->
            if (suppressSwitchCallback)
                return@setOnCheckedChangeListener
            if (isChecked) openSdr() else stopSdr()
        }

        val savedRfAudio = prefs.getBoolean("rfAudio", false)
        BmrfService.rfAudioEnabled.value = savedRfAudio
        rfAudioSwitch.isChecked = savedRfAudio
        rfAudioSwitch.setOnCheckedChangeListener { _, isChecked ->
            BmrfService.rfAudioEnabled.value = isChecked
            prefs.edit().putBoolean("rfAudio", isChecked).apply()
        }
        val savedRfForward = prefs.getBoolean("rfForward", false)
        BmrfService.rfForwardEnabled.value = savedRfForward
        rfForwardSwitch.isChecked = savedRfForward
        rfForwardSwitch.setOnCheckedChangeListener { _, isChecked ->
            BmrfService.rfForwardEnabled.value = isChecked
            prefs.edit().putBoolean("rfForward", isChecked).apply()
        }

        findViewById<Button>(R.id.startButton).setOnClickListener { startGateway() }
        findViewById<Button>(R.id.stopButton).setOnClickListener { stopGateway() }
        findViewById<Button>(R.id.openSdrButton).setOnClickListener { openSdr() }
        findViewById<Button>(R.id.applyTgsButton).setOnClickListener { applyTalkgroups() }

        CallsignDb.load(this)
        updateContactStatus()
        contactsPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null)
                importContacts(uri)
        }
        findViewById<Button>(R.id.importContactsButton).setOnClickListener {
            contactsPicker.launch(arrayOf("text/*", "text/comma-separated-values", "text/tab-separated-values", "*/*"))
        }

        loadConnectionFields()
        requestNotificationPermission()
        observeCore()

        renderStatus(null)
        renderCurrentCall(null)

        recordIq = intent?.getBooleanExtra("recordIq", false) ?: false
        captureOnly = intent?.getBooleanExtra("captureOnly", false) ?: false
        launchFreqHz = intent?.getIntExtra("sdrFreq", 0) ?: 0
        launchRateHz = intent?.getIntExtra("sdrRate", 0) ?: 0
        if (launchFreqHz > 0)
            sdrFreqInput.setText(launchFreqHz.toString())
        if (launchRateHz > 0)
            sdrRateInput.setText(launchRateHz.toString())
        val launchGain = intent?.getIntExtra("sdrGain", -1) ?: -1
        if (launchGain >= 0)
            sdrGainInput.setText(launchGain.toString())

        if (intent?.getBooleanExtra("rfAudio", false) == true) {
            BmrfService.rfAudioEnabled.value = true
            rfAudioSwitch.isChecked = true
        }
        if (intent?.hasExtra("rfTimeslot") == true) {
            val slot = intent.getIntExtra("rfTimeslot", 1)
            rfTimeslotInput.setText(slot.toString())
            BmrfService.rfTimeslot.value = slot
        }
        if (intent?.getBooleanExtra("openSdr", false) == true)
            openSdr()
    }

    // Tapping anywhere outside the focused field clears focus and hides the
    // soft keyboard, so it is not re-summoned whenever the screen is touched.
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (ev.action == MotionEvent.ACTION_DOWN) {
            val focused = currentFocus
            if (focused is EditText) {
                val rect = Rect()
                focused.getGlobalVisibleRect(rect)
                if (!rect.contains(ev.rawX.toInt(), ev.rawY.toInt())) {
                    focused.clearFocus()
                    val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                    imm.hideSoftInputFromWindow(focused.windowToken, 0)
                }
            }
        }
        return super.dispatchTouchEvent(ev)
    }

    private fun loadConnectionFields() {
        hostInput.setText(prefs.getString("host", "4602.master.brandmeister.network"))
        portInput.setText(prefs.getString("port", "62031"))
        idInput.setText(prefs.getString("id", "4615543"))
        callsignInput.setText(prefs.getString("callsign", ""))
        passwordInput.setText(prefs.getString("password", ""))
        apiKeyInput.setText(prefs.getString("apiKey", ""))
        ts1Input.setText(prefs.getString("ts1", ""))
        ts2Input.setText(prefs.getString("ts2", ""))
        audioTalkgroupsInput.setText(prefs.getString("audioTalkgroups", ""))
        BmrfService.audioTalkgroups.value = audioTalkgroupsInput.text.toString().trim()
    }

    private fun saveConnectionFields() {
        prefs.edit()
            .putString("host", hostInput.text.toString())
            .putString("port", portInput.text.toString())
            .putString("id", idInput.text.toString())
            .putString("callsign", callsignInput.text.toString())
            .putString("password", passwordInput.text.toString())
            .putString("apiKey", apiKeyInput.text.toString())
            .putString("ts1", ts1Input.text.toString())
            .putString("ts2", ts2Input.text.toString())
            .putString("audioTalkgroups", audioTalkgroupsInput.text.toString())
            .apply()
    }

    private fun parseTalkgroups(text: String): List<Int> =
        text.split(',', ' ', ';').mapNotNull { it.trim().toIntOrNull() }.filter { it > 0 }

    private fun applyTalkgroups() {
        val apiKey = apiKeyInput.text.toString().trim()
        val id = idInput.text.toString().toIntOrNull() ?: 0
        if (apiKey.isEmpty()) {
            Toast.makeText(this, "Enter your BrandMeister API key first", Toast.LENGTH_LONG).show()
            return
        }
        if (id <= 0) {
            Toast.makeText(this, "Enter the Repeater DMR ID first", Toast.LENGTH_LONG).show()
            return
        }
        saveConnectionFields()
        val desired = mapOf(
            1 to parseTalkgroups(ts1Input.text.toString()),
            2 to parseTalkgroups(ts2Input.text.toString())
        )
        Toast.makeText(this, "Applying talkgroups…", Toast.LENGTH_SHORT).show()
        lifecycleScope.launch {
            val result = TalkgroupManager.apply(apiKey, id.toLong(), desired)
            Toast.makeText(this@MainActivity, result.message, Toast.LENGTH_LONG).show()
        }
    }

    private fun updateContactStatus() {
        val count = CallsignDb.count()
        contactStatusText.text = if (count > 0)
            getString(R.string.contacts_loaded, count)
        else
            getString(R.string.contacts_none)
    }

    private fun importContacts(uri: Uri) {
        Toast.makeText(this, "Importing contacts…", Toast.LENGTH_SHORT).show()
        lifecycleScope.launch {
            val imported = withContext(Dispatchers.IO) {
                CallsignDb.importFrom(this@MainActivity, uri)
            }
            updateContactStatus()
            if (imported > 0) {
                adapter.notifyDataSetChanged()
                renderCurrentCall(BmrfService.currentCall.value)
                Toast.makeText(
                    this@MainActivity,
                    getString(R.string.contacts_imported, imported),
                    Toast.LENGTH_LONG
                ).show()
            } else {
                Toast.makeText(
                    this@MainActivity,
                    getString(R.string.contacts_import_failed),
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    private fun startGateway() {
        saveConnectionFields()
        val intent = Intent(this, BmrfService::class.java).apply {
            putExtra(BmrfService.EXTRA_HOST, hostInput.text.toString().ifBlank { "4602.master.brandmeister.network" })
            putExtra(BmrfService.EXTRA_PORT, portInput.text.toString().toIntOrNull() ?: 62031)
            putExtra(BmrfService.EXTRA_REPEATER_ID, idInput.text.toString().toIntOrNull() ?: 0)
            putExtra(BmrfService.EXTRA_CALLSIGN, callsignInput.text.toString().ifBlank { "BMRF" })
            putExtra(BmrfService.EXTRA_PASSWORD, passwordInput.text.toString())
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopGateway() {
        stopService(Intent(this, BmrfService::class.java))
    }

    private fun openSdr() {
        BmrfService.rfTimeslot.value = rfTimeslotInput.text.toString().toIntOrNull() ?: 1
        val intent = Intent(this, BmrfService::class.java).apply {
            action = BmrfService.ACTION_OPEN_SDR
            putExtra(
                BmrfService.EXTRA_SDR_CENTER,
                sdrFreqInput.text.toString().toIntOrNull()?.takeIf { it > 0 } ?: 432750000
            )
            putExtra(
                BmrfService.EXTRA_SDR_RATE,
                sdrRateInput.text.toString().toIntOrNull()?.takeIf { it in 100000..3200000 } ?: 240000
            )
            putExtra(BmrfService.EXTRA_SDR_GAIN, sdrGainInput.text.toString().toIntOrNull() ?: 0)
            putExtra(BmrfService.EXTRA_SDR_PPM, sdrPpmInput.text.toString().toIntOrNull() ?: 0)
            putExtra(BmrfService.EXTRA_RECORD_IQ, recordIq)
            putExtra(BmrfService.EXTRA_CAPTURE_ONLY, captureOnly)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopSdr() {
        startService(Intent(this, BmrfService::class.java).setAction(BmrfService.ACTION_CLOSE_SDR))
    }

    private fun observeCore() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { BmrfService.status.collect(::renderStatus) }
                launch { BmrfService.currentCall.collect(::renderCurrentCall) }
                launch { BmrfService.history.collect(adapter::submit) }
                launch {
                    BmrfService.sdrMessage.collect { message ->
                        if (!message.isNullOrEmpty())
                            sdrStatusText.text = message
                    }
                }
            }
        }
    }

    private fun renderStatus(status: NetworkStatus?) {
        if (status == null) {
            stateChip.text = getString(R.string.status_idle)
            stateChip.backgroundTintList = tint(R.color.state_off)
            callTitleText.text = getString(R.string.current_call_idle)
            callDetailText.text = ""
            audioStatusText.text = "AUDIO  muted"
            decoderText.text = "decoder: -"
            networkDetailText.text = ""
            statsText.text = ""
            sdrStatusText.text = getString(R.string.sdr_idle)
            return
        }

        stateChip.text = status.state
        stateChip.backgroundTintList = tint(colorForState(status.state))

        decoderText.text = "decoder: ${status.decoder}   queue: ${status.audioQueue}"
        audioStatusText.text = when {
            !status.audioEnabled -> "AUDIO  muted"
            status.audioPlaying -> "\u25B6 PLAYING"
            else -> "AUDIO  idle"
        }

        networkDetailText.text = buildString {
            append("MASTER    ").append(status.master).append(':').append(status.port).append('\n')
            if (status.bmPeer.isNotEmpty())
                append("PEER      ").append(status.bmPeer).append('\n')
            append("REPEATER  ").append(status.repeaterId).append('\n')
            append("LATENCY   ").append(status.latencyMs).append(" ms\n")
            append("LOGIN     ").append(if (status.loggedIn) "yes" else "no").append('\n')
            append("ACTIVE    ").append(status.activeCalls)
        }

        val total = status.totalFrames + status.totalLost
        val loss = if (total > 0) status.totalLost * 100.0 / total else 0.0
        statsText.text = buildString {
            append("CALLS     ").append(status.totalCalls).append('\n')
            append("FRAMES    ").append(status.totalFrames).append('\n')
            append("LOST      ").append(status.totalLost).append('\n')
            append("LOSS      ").append(String.format(Locale.US, "%.2f%%", loss)).append('\n')
            append("AUDIO LAT ").append(status.audioLatencyMs).append(" ms")
        }

        if (rfReceiveSwitch.isChecked != status.sdrOpen) {
            suppressSwitchCallback = true
            rfReceiveSwitch.isChecked = status.sdrOpen
            suppressSwitchCallback = false
        }

        if (status.sdrOpen) {
            sdrStatusText.text = buildString {
                append("open  ").append(status.sdrCenterHz).append(" Hz  @")
                append(status.sdrSampleRate).append(" Hz\n")
                append("level  ").append(String.format(Locale.US, "%.1f", status.rfLevelDb))
                append(" dBFS   snr ").append(String.format(Locale.US, "%.1f", status.rfSnrDb))
                append(" dB\n")
                append("samples=").append(status.sdrSamples)
                append("  rms=").append(String.format(Locale.US, "%.1f", status.sdrRms))
            }
        }

        if (audioSwitch.isChecked != status.audioEnabled)
            audioSwitch.isChecked = status.audioEnabled
    }

    private fun renderCurrentCall(call: DmrCall?) {
        if (call == null) {
            callTitleText.text = getString(R.string.current_call_idle)
            callDetailText.text = ""
            return
        }
        val destination = if (call.callType.equals("Group", ignoreCase = true)) {
            "TG ${call.destId}"
        } else {
            "ID ${CallsignDb.display(call.destId)}"
        }
        callTitleText.text = "$destination \u00B7 ${call.callType.uppercase(Locale.US)}"

        val totalSeconds = call.durationMs / 1000
        val duration = String.format(Locale.US, "%02d:%02d", totalSeconds / 60, totalSeconds % 60)
        callDetailText.text = buildString {
            append("Source ID    ").append(CallsignDb.display(call.sourceId)).append('\n')
            append("Destination  ").append(destination).append('\n')
            append("Timeslot     ").append(call.timeslot ?: "-").append('\n')
            call.colorCode?.let { append("Color Code   ").append(it).append('\n') }
            call.flco?.let {
                append("FLCO         0x")
                    .append(String.format(Locale.US, "%02X", it)).append('\n')
            }
            call.serviceOptions?.let {
                append("Svc Options  0x")
                    .append(String.format(Locale.US, "%02X", it)).append('\n')
            }
            append("Duration     ").append(duration).append('\n')
            append("Frames       ").append(call.frameCount).append('\n')
            append("Lost         ").append(call.lostFrameCount).append('\n')
            call.rssi?.let {
                append("RSSI         ").append(String.format(Locale.US, "%.1f", it)).append(" dB\n")
            }
            call.snr?.let {
                append("SNR          ").append(String.format(Locale.US, "%.1f", it)).append(" dB\n")
            }
            call.ber?.let {
                append("BER          ").append(String.format(Locale.US, "%.2f", it)).append(" %\n")
            }
            append("Path         ").append(call.source)
        }
    }

    private fun colorForState(state: String): Int = when (state) {
        "ACTIVE" -> R.color.state_ok
        "CONNECTING", "REGISTERED", "RECONNECTING" -> R.color.state_warn
        else -> R.color.state_off
    }

    private fun tint(colorRes: Int): ColorStateList =
        ColorStateList.valueOf(ContextCompat.getColor(this, colorRes))

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
    }
}
