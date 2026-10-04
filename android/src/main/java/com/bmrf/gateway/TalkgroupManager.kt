package com.bmrf.gateway

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.net.HttpURLConnection
import java.net.URL

// Manages the hotspot's static BrandMeister talkgroups via the official
// BrandMeister API v2 (https://help.brandmeister.network/hotspots/api-manager/).
// Authentication uses a personal API key generated in the BrandMeister
// dashboard (SelfCare -> API keys), sent as an HTTP Bearer token.
object TalkgroupManager {

    private const val BASE = "https://api.brandmeister.network/v2/device"

    data class Result(val ok: Boolean, val message: String)

    private fun open(method: String, path: String, apiKey: String): HttpURLConnection {
        val conn = URL("$BASE/$path").openConnection() as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = 10_000
        conn.readTimeout = 10_000
        conn.setRequestProperty("Authorization", "Bearer $apiKey")
        conn.setRequestProperty("Accept", "application/json")
        return conn
    }

    private fun current(apiKey: String, id: Long): List<Pair<Int, Int>> {
        val conn = open("GET", "$id/talkgroup", apiKey)
        try {
            if (conn.responseCode !in 200..299)
                throw IllegalStateException("list failed (HTTP ${conn.responseCode})")
            val body = conn.inputStream.bufferedReader().readText()
            val array = JSONArray(body)
            val out = ArrayList<Pair<Int, Int>>(array.length())
            for (i in 0 until array.length()) {
                val item = array.optJSONObject(i) ?: continue
                val group = item.optJSONObject("talkgroup")
                val tg = group?.optInt("talkgroup", -1) ?: item.optInt("talkgroup", -1)
                out.add(item.optInt("slot", 0) to tg)
            }
            return out
        } finally {
            conn.disconnect()
        }
    }

    private fun add(apiKey: String, id: Long, slot: Int, tg: Int) {
        val conn = open("POST", "$id/talkgroup", apiKey)
        conn.doOutput = true
        conn.setRequestProperty("Content-Type", "application/json")
        try {
            conn.outputStream.use { it.write("{\"slot\":$slot,\"group\":$tg}".toByteArray()) }
            conn.responseCode
        } finally {
            conn.disconnect()
        }
    }

    private fun remove(apiKey: String, id: Long, slot: Int, tg: Int) {
        val conn = open("DELETE", "$id/talkgroup/$slot/$tg", apiKey)
        try {
            conn.responseCode
        } finally {
            conn.disconnect()
        }
    }

    // Make the hotspot's static talkgroups match `desired` (slot -> talkgroups).
    // Slots present in the map are fully synchronised (extras removed).
    suspend fun apply(apiKey: String, id: Long, desired: Map<Int, List<Int>>): Result =
        withContext(Dispatchers.IO) {
            try {
                val existing = current(apiKey, id)
                var added = 0
                var removed = 0
                for ((slot, tgs) in desired) {
                    val want = tgs.toSet()
                    for (tg in want) {
                        if (existing.none { it.first == slot && it.second == tg }) {
                            add(apiKey, id, slot, tg); added++
                        }
                    }
                    for ((s, tg) in existing) {
                        if (s == slot && tg !in want) {
                            remove(apiKey, id, slot, tg); removed++
                        }
                    }
                }
                Result(true, "Talkgroups updated: +$added -$removed")
            } catch (e: Exception) {
                Result(false, "Talkgroup error: ${e.message}")
            }
        }
}
