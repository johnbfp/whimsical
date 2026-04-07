package com.whimsical.agent.data

import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject
import java.util.concurrent.TimeUnit

class TaskStreamClient(
    private val baseWsUrl: String,
    private val client: OkHttpClient = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)
        .build(),
) {
    private var socket: WebSocket? = null
    private var currentTaskId: String? = null

    var onEvent: ((AgentEvent) -> Unit)? = null
    var onRawEvent: ((String) -> Unit)? = null
    var onError: ((Throwable) -> Unit)? = null
    var onClosed: (() -> Unit)? = null

    fun subscribe(taskId: String) {
        close()
        currentTaskId = taskId

        val request = Request.Builder()
            .url("$baseWsUrl/agents/stream?task_id=$taskId")
            .build()

        socket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                Log.d(TAG, "WebSocket opened for task=$taskId")
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                onRawEvent?.invoke(text)
                try {
                    val event = JSONObject(text).toAgentEvent()
                    onEvent?.invoke(event)
                } catch (e: Exception) {
                    Log.w(TAG, "Failed to parse event: ${e.message}")
                }
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                onMessage(webSocket, bytes.utf8())
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                Log.e(TAG, "WebSocket failure: ${t.message}")
                onError?.invoke(t)
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(code, reason)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                Log.d(TAG, "WebSocket closed: $code $reason")
                onClosed?.invoke()
            }
        })
    }

    fun close() {
        socket?.close(1000, "normal")
        socket = null
        currentTaskId = null
    }

    companion object {
        private const val TAG = "TaskStreamClient"
    }
}
