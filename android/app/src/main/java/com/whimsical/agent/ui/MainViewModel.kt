package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.TaskStreamClient
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

class MainViewModel(
    private val baseHttpUrl: String,
    private val streamClient: TaskStreamClient,
    private val client: OkHttpClient = OkHttpClient()
) : ViewModel() {

    private val _events = MutableStateFlow<List<String>>(emptyList())
    val events: StateFlow<List<String>> = _events

    private val _taskId = MutableStateFlow("")
    val taskId: StateFlow<String> = _taskId

    fun createTask(userId: String, prompt: String) {
        viewModelScope.launch {
            val body = """
                {"tenant_id":"default","user_id":"$userId","prompt":"$prompt"}
            """.trimIndent().toRequestBody("application/json".toMediaType())

            val req = Request.Builder()
                .url("$baseHttpUrl/agents/tasks")
                .post(body)
                .build()

            client.newCall(req).execute().use { resp ->
                val text = resp.body?.string().orEmpty()
                val id = Regex("\"task_id\"\\s*:\\s*\"([^\"]+)\"")
                    .find(text)?.groupValues?.get(1).orEmpty()

                _taskId.value = id
                bindTaskStream(id)
            }
        }
    }

    fun bindTaskStream(taskId: String) {
        if (taskId.isBlank()) return
        streamClient.subscribe(taskId) { eventJson ->
            _events.value = listOf(eventJson) + _events.value
        }
    }

    fun cancelTask() {
        val id = _taskId.value
        if (id.isBlank()) return
        viewModelScope.launch {
            val req = Request.Builder()
                .url("$baseHttpUrl/agents/tasks/$id/cancel")
                .post("".toRequestBody(null))
                .build()
            client.newCall(req).execute().close()
        }
    }

    override fun onCleared() {
        streamClient.close()
        super.onCleared()
    }
}
