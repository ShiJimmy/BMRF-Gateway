package com.bmrf.gateway

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class CallAdapter : RecyclerView.Adapter<CallAdapter.ViewHolder>() {

    private val items = ArrayList<CallHistoryEntry>()
    private val clock = SimpleDateFormat("HH:mm:ss", Locale.US)

    fun submit(entries: List<CallHistoryEntry>) {
        items.clear()
        items.addAll(entries)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_call, parent, false)
        return ViewHolder(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val entry = items[position]
        val call = entry.call
        val destination = if (call.callType.equals("Group", ignoreCase = true)) {
            "TG${call.destId}"
        } else {
            "ID ${CallsignDb.display(call.destId)}"
        }
        holder.title.text = "${call.callType}  ${CallsignDb.display(call.sourceId)} \u2192 $destination"
        holder.subtitle.text =
            "${clock.format(Date(entry.endedAtMs))}  TS${call.timeslot ?: "-"}  " +
                "${formatDuration(call.durationMs)}  frames=${call.frameCount}  " +
                "lost=${call.lostFrameCount}  ${call.source}"
    }

    private fun formatDuration(millis: Long): String {
        val totalSeconds = millis / 1000
        return String.format(Locale.US, "%02d:%02d", totalSeconds / 60, totalSeconds % 60)
    }

    class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
        val title: TextView = view.findViewById(R.id.callTitle)
        val subtitle: TextView = view.findViewById(R.id.callSubtitle)
    }
}
