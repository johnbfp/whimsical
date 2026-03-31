package com.whimsical.agent.data

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString

class TaskStreamClient(
    private val baseWsUrl: String,
    private val client: OkHttpClient = OkHttpClient()
) {
    private var socket: WebSocket? = null

    fun subscribe(taskId: String, onEvent: (String) -> Unit) {
        val request = Request.Builder()
            .url("$baseWsUrl/agents/stream?task_id=$taskId")
            .build()

        socket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) {
                onEvent(text)
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                onEvent(bytes.utf8())
            }
        })
    }

    fun close() {
        socket?.close(1000, "normal")
        socket = null
    }
}
