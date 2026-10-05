package com.bmrf.gateway

import org.json.JSONArray
import org.json.JSONObject

data class DmrCall(
    val callId: Long,
    val sourceId: Long,
    val destId: Long,
    val callType: String,
    val state: String,
    val source: String,
    val timeslot: Int?,
    val colorCode: Int?,
    val flco: Int?,
    val serviceOptions: Int?,
    val frameCount: Long,
    val lostFrameCount: Long,
    val durationMs: Long,
    val rssi: Double?,
    val snr: Double?,
    val ber: Double?
)

data class DmrFrameInfo(
    val sourceId: Long,
    val destId: Long,
    val timeslot: Int,
    val dataType: String,
    val sequence: Long,
    val hasVoice: Boolean
)

data class CoreEvent(
    val type: String,
    val message: String,
    val timestampNs: Long,
    val call: DmrCall?,
    val frame: DmrFrameInfo?
)

data class NetworkStatus(
    val state: String,
    val master: String,
    val bmPeer: String,
    val port: Int,
    val repeaterId: Long,
    val latencyMs: Long,
    val loggedIn: Boolean,
    val activeCalls: Int,
    val decoder: String,
    val audioEnabled: Boolean,
    val audioPlaying: Boolean,
    val audioQueue: Int,
    val audioLatencyMs: Long,
    val totalCalls: Long,
    val totalFrames: Long,
    val totalLost: Long,
    val sdrOpen: Boolean,
    val sdrCenterHz: Long,
    val sdrSampleRate: Long,
    val sdrSamples: Long,
    val sdrRms: Double,
    val rfAudioEnabled: Boolean,
    val rfForwardEnabled: Boolean,
    val rfForwarded: Long,
    val rfLevelDb: Double,
    val rfSnrDb: Double
)

data class CoreSnapshot(
    val status: NetworkStatus?,
    val events: List<CoreEvent>
)

data class CallHistoryEntry(
    val call: DmrCall,
    val endedAtMs: Long
)

object SnapshotParser {

    fun parse(json: String): CoreSnapshot {
        val root = JSONObject(json)
        val status = root.optJSONObject("status")?.let(::parseStatus)
        val array = root.optJSONArray("events") ?: JSONArray()
        val events = ArrayList<CoreEvent>(array.length())
        for (index in 0 until array.length()) {
            array.optJSONObject(index)?.let { events.add(parseEvent(it)) }
        }
        return CoreSnapshot(status, events)
    }

    private fun parseStatus(json: JSONObject) = NetworkStatus(
        state = json.optString("state", "DISCONNECTED"),
        master = json.optString("master", ""),
        bmPeer = json.optString("bmPeer", ""),
        port = json.optInt("port", 62031),
        repeaterId = json.optLong("repeaterId", 0L),
        latencyMs = json.optLong("latencyMs", 0L),
        loggedIn = json.optBoolean("loggedIn", false),
        activeCalls = json.optInt("activeCalls", 0),
        decoder = json.optString("decoder", "none"),
        audioEnabled = json.optBoolean("audioEnabled", false),
        audioPlaying = json.optBoolean("audioPlaying", false),
        audioQueue = json.optInt("audioQueue", 0),
        audioLatencyMs = json.optLong("audioLatencyMs", 0L),
        totalCalls = json.optLong("totalCalls", 0L),
        totalFrames = json.optLong("totalFrames", 0L),
        totalLost = json.optLong("totalLost", 0L),
        sdrOpen = json.optBoolean("sdrOpen", false),
        sdrCenterHz = json.optLong("sdrCenterHz", 0L),
        sdrSampleRate = json.optLong("sdrSampleRate", 0L),
        sdrSamples = json.optLong("sdrSamples", 0L),
        sdrRms = json.optDouble("sdrRms", 0.0),
        rfAudioEnabled = json.optBoolean("rfAudioEnabled", false),
        rfForwardEnabled = json.optBoolean("rfForwardEnabled", false),
        rfForwarded = json.optLong("rfForwarded", 0L),
        rfLevelDb = json.optDouble("rfLevelDb", 0.0),
        rfSnrDb = json.optDouble("rfSnrDb", 0.0)
    )

    private fun parseCall(json: JSONObject) = DmrCall(
        callId = json.optLong("callId"),
        sourceId = json.optLong("sourceId"),
        destId = json.optLong("destId"),
        callType = json.optString("callType", "Unknown"),
        state = json.optString("state", "Idle"),
        source = json.optString("source", "Unknown"),
        timeslot = json.optNullableInt("timeslot"),
        colorCode = json.optNullableInt("colorCode"),
        flco = json.optNullableInt("flco"),
        serviceOptions = json.optNullableInt("serviceOptions"),
        frameCount = json.optLong("frameCount"),
        lostFrameCount = json.optLong("lostFrameCount"),
        durationMs = json.optLong("durationMs"),
        rssi = json.optNullableDouble("rssi"),
        snr = json.optNullableDouble("snr"),
        ber = json.optNullableDouble("ber")
    )

    private fun parseFrame(json: JSONObject) = DmrFrameInfo(
        sourceId = json.optLong("sourceId"),
        destId = json.optLong("destId"),
        timeslot = json.optInt("timeslot"),
        dataType = json.optString("dataType", "Idle"),
        sequence = json.optLong("sequence"),
        hasVoice = json.optBoolean("hasVoice")
    )

    private fun parseEvent(json: JSONObject) = CoreEvent(
        type = json.optString("type", "Unknown"),
        message = json.optString("message", ""),
        timestampNs = json.optLong("timestampNs"),
        call = json.optJSONObject("call")?.let(::parseCall),
        frame = json.optJSONObject("frame")?.let(::parseFrame)
    )

    private fun JSONObject.optNullableInt(key: String): Int? =
        if (isNull(key)) null else optInt(key)

    private fun JSONObject.optNullableDouble(key: String): Double? =
        if (isNull(key)) null else optDouble(key)
}
