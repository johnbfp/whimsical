package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.AgentEvent
import com.whimsical.agent.data.AgentRepository
import com.whimsical.agent.data.Plan
import com.whimsical.agent.data.TaskInfo
import com.whimsical.agent.data.TaskState
import com.whimsical.agent.data.ToolStep
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class MainViewModel(private val repo: AgentRepository) : ViewModel() {

    private val _events = MutableStateFlow<List<AgentEvent>>(emptyList())
    val events: StateFlow<List<AgentEvent>> = _events

    private val _taskId = MutableStateFlow("")
    val taskId: StateFlow<String> = _taskId

    private val _taskState = MutableStateFlow(TaskState.CREATED)
    val taskState: StateFlow<TaskState> = _taskState

    private val _taskResult = MutableStateFlow<String?>(null)
    val taskResult: StateFlow<String?> = _taskResult

    private val _currentPlan = MutableStateFlow<Plan?>(null)
    val currentPlan: StateFlow<Plan?> = _currentPlan

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading

    // ── Task operations ─────────────────────────────────────────────

    fun createTask(userId: String, prompt: String) {
        viewModelScope.launch {
            _loading.value = true
            _error.value = null
            _events.value = emptyList()
            _taskResult.value = null
            _currentPlan.value = null
            try {
                val task = repo.createTask("default", userId, prompt)
                _taskId.value = task.taskId
                _taskState.value = task.state
                bindTaskStream(task.taskId)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun refreshTask() {
        val id = _taskId.value
        if (id.isBlank()) return
        viewModelScope.launch {
            try {
                val task = repo.getTask(id)
                _taskState.value = task.state
                _taskResult.value = task.result
                if (task.plan != null) _currentPlan.value = task.plan
            } catch (e: Exception) {
                _error.value = e.message
            }
        }
    }

    fun cancelTask() {
        val id = _taskId.value
        if (id.isBlank()) return
        viewModelScope.launch {
            try {
                repo.cancelTask(id)
                _taskState.value = TaskState.CANCELLED
            } catch (e: Exception) {
                _error.value = e.message
            }
        }
    }

    // ── Stream ──────────────────────────────────────────────────────

    private fun bindTaskStream(taskId: String) {
        if (taskId.isBlank()) return
        repo.stream.onEvent = { event -> dispatchEvent(event) }
        repo.stream.onError = { t -> _error.value = t.message }
        repo.subscribeEvents(taskId)
    }

    private fun dispatchEvent(event: AgentEvent) {
        _events.value = listOf(event) + _events.value

        val payload = event.payload
        (payload["state"] as? String)?.let { _taskState.value = TaskState.from(it) }
        (payload["result"] as? String)?.let { _taskResult.value = it }
        (payload["error"] as? String)?.let { _error.value = it }

        // Extract plan from event payload if present
        @Suppress("UNCHECKED_CAST")
        (payload["plan"] as? Map<String, Any?>)?.let { planMap ->
            val steps = (planMap["execution_steps"] as? List<*>)?.mapNotNull { raw ->
                val m = raw as? Map<*, *> ?: return@mapNotNull null
                ToolStep(
                    toolName = m["tool_name"]?.toString() ?: "",
                    input = (m["input"] as? Map<String, Any>) ?: emptyMap(),
                )
            } ?: emptyList()
            _currentPlan.value = Plan(
                taskBreakdown = (planMap["task_breakdown"] as? List<String>) ?: emptyList(),
                selectedTools = (planMap["selected_tools"] as? List<String>) ?: emptyList(),
                executionSteps = steps,
                reflectionHint = (planMap["reflection_hint"] as? String) ?: "",
            )
        }
    }

    override fun onCleared() {
        repo.closeStream()
        super.onCleared()
    }

    // ── Factory ─────────────────────────────────────────────────────

    class Factory(private val repo: AgentRepository) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = MainViewModel(repo) as T
    }
}
