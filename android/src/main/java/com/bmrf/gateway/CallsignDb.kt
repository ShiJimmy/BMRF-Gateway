package com.bmrf.gateway

import android.content.Context
import android.net.Uri
import android.util.Log
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.util.concurrent.ConcurrentHashMap

/**
 * DMR ID -> callsign lookup, populated from a user-imported contact list.
 *
 * The importer accepts a CSV/TSV whose first two columns are the numeric
 * RADIO_ID and the CALLSIGN (extra columns are ignored), e.g. the common
 * "output.csv" exports:
 *
 *     RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,...
 *     4540001,VR2WNX,Mingrui,...
 *
 * It is normalised into a compact "id<TAB>callsign" file in the app's private
 * storage and loaded at startup, so the raw (potentially large) export does not
 * need to be re-parsed on every launch.
 */
object CallsignDb {

    private const val TAG = "BMRF/calls"
    private const val FILE_NAME = "callsigns.tsv"

    private val map = ConcurrentHashMap<Long, String>()

    @Volatile
    private var loaded = false

    fun count(): Int = map.size

    /** Load the persisted list (once). Safe to call from onCreate. */
    fun load(context: Context) {
        if (loaded)
            return
        loaded = true
        try {
            val file = File(context.filesDir, FILE_NAME)
            if (!file.exists())
                return
            BufferedReader(InputStreamReader(file.inputStream(), Charsets.UTF_8)).use { reader ->
                reader.forEachLine { line ->
                    val tab = line.indexOf('\t')
                    if (tab > 0) {
                        val id = line.substring(0, tab).toLongOrNull()
                        val callsign = line.substring(tab + 1).trim()
                        if (id != null && id > 0 && callsign.isNotEmpty())
                            map[id] = callsign
                    }
                }
            }
            Log.i(TAG, "loaded ${map.size} callsigns")
        } catch (e: Exception) {
            Log.w(TAG, "load failed: ${e.message}")
        }
    }

    /**
     * Parse [uri] (CSV/TSV, first two columns = RADIO_ID, CALLSIGN), persist a
     * compact copy and reload. Returns the number of entries imported, or -1 on
     * failure. Call from a background thread.
     */
    fun importFrom(context: Context, uri: Uri): Int {
        val fresh = HashMap<Long, String>()
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                BufferedReader(InputStreamReader(input, Charsets.UTF_8)).use { reader ->
                    reader.forEachLine { raw ->
                        val line = raw.trim()
                        if (line.isEmpty())
                            return@forEachLine
                        val sep = separatorOf(line)
                        val parts = line.split(sep)
                        if (parts.size < 2)
                            return@forEachLine
                        val id = parts[0].trim().trim('"').toLongOrNull()
                            ?: return@forEachLine // header or malformed row
                        val callsign = parts[1].trim().trim('"')
                        if (id <= 0 || callsign.isEmpty())
                            return@forEachLine
                        if (callsign.equals("CALLSIGN", ignoreCase = true))
                            return@forEachLine
                        fresh[id] = callsign
                    }
                }
            } ?: return -1
        } catch (e: Exception) {
            Log.w(TAG, "import failed: ${e.message}")
            return -1
        }

        if (fresh.isEmpty())
            return -1

        try {
            File(context.filesDir, FILE_NAME).bufferedWriter(Charsets.UTF_8).use { writer ->
                for ((id, callsign) in fresh) {
                    writer.write(id.toString())
                    writer.write("\t")
                    writer.write(callsign)
                    writer.newLine()
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "persist failed: ${e.message}")
        }

        map.clear()
        map.putAll(fresh)
        Log.i(TAG, "imported ${fresh.size} callsigns")
        return fresh.size
    }

    /** "1234567 AB1CDE" when known, otherwise just "1234567". */
    fun display(id: Long): String {
        val callsign = map[id]
        return if (callsign.isNullOrEmpty()) id.toString() else "$id $callsign"
    }

    /** Callsign only, or null when the ID is not in the list. */
    fun callsign(id: Long): String? = map[id]

    private fun separatorOf(line: String): Char = when {
        line.contains('\t') -> '\t'
        line.contains(',') -> ','
        line.contains(';') -> ';'
        else -> ' '
    }
}
