package com.whimsical.agent.data

import org.json.JSONArray
import org.json.JSONObject

// ── Task ────────────────────────────────────────────────────────────────

enum class TaskState {
    CREATED, PLANNING, RUNNING, WAITING_TOOL, COMPLETED, FAILED, CANCELLED;

    val isTerminal get() = this in setOf(COMPLETED, FAILED, CANCELLED)

    companion object {
        fun from(raw: String): TaskState =
            entries.firstOrNull { it.name.equals(raw, ignoreCase = true) } ?: CREATED
    }
}

data class ToolStep(
    val toolName: String,
    val input: Map<String, Any> = emptyMap(),
)

data class Plan(
    val taskBreakdown: List<String>,
    val selectedTools: List<String>,
    val executionSteps: List<ToolStep>,
    val reflectionHint: String,
)

data class TaskInfo(
    val taskId: String,
    val tenantId: String,
    val userId: String,
    val prompt: String,
    val state: TaskState,
    val plan: Plan? = null,
    val result: String? = null,
    val error: String? = null,
    val modelProvider: String = "local",
    val modelName: String = "",
    val createdAt: String = "",
    val updatedAt: String = "",
)

// ── Agent Event ─────────────────────────────────────────────────────────

data class AgentEvent(
    val eventType: String,
    val taskId: String,
    val payload: Map<String, Any?> = emptyMap(),
    val timestamp: String = "",
)

// ── Memory ──────────────────────────────────────────────────────────────

data class MemoryRecallRequest(
    val tenantId: String = "default",
    val userId: String,
    val query: String,
    val topK: Int = 5,
)

data class MemoryWriteRequest(
    val tenantId: String = "default",
    val userId: String = "anonymous",
    val key: String,
    val value: String,
)

data class MemoryEntry(
    val key: String,
    val value: String,
)

// ── Plugin ──────────────────────────────────────────────────────────────

data class PluginInfo(
    val pluginId: String,
    val version: String,
    val toolName: String,
    val permissions: List<String>,
    val timeoutMs: Int = 4000,
    val enabled: Boolean = false,
)

// ── Model ───────────────────────────────────────────────────────────────

data class ModelSwitchRequest(
    val provider: String,
    val modelName: String,
)

data class ModelInfo(
    val provider: String,
    val modelName: String,
)

// ── JSON helpers ────────────────────────────────────────────────────────

fun JSONObject.toMap(): Map<String, Any?> {
    val map = mutableMapOf<String, Any?>()
    keys().forEach { k ->
        map[k] = when (val v = get(k)) {
            is JSONObject -> v.toMap()
            is JSONArray -> v.toList()
            JSONObject.NULL -> null
            else -> v
        }
    }
    return map
}

fun JSONArray.toList(): List<Any?> =
    (0 until length()).map { i ->
        when (val v = get(i)) {
            is JSONObject -> v.toMap()
            is JSONArray -> v.toList()
            JSONObject.NULL -> null
            else -> v
        }
    }

fun JSONObject.toStringList(key: String): List<String> {
    val arr = optJSONArray(key) ?: return emptyList()
    return (0 until arr.length()).map { arr.getString(it) }
}

fun JSONObject.toToolSteps(key: String): List<ToolStep> {
    val arr = optJSONArray(key) ?: return emptyList()
    return (0 until arr.length()).map { i ->
        val o = arr.getJSONObject(i)
        ToolStep(
            toolName = o.optString("tool_name", ""),
            input = o.optJSONObject("input")?.toMap()?.mapValues { it.value ?: "" } ?: emptyMap()
        )
    }
}

fun JSONObject.toPlan(): Plan? {
    val p = optJSONObject("plan") ?: return null
    return Plan(
        taskBreakdown = p.toStringList("task_breakdown"),
        selectedTools = p.toStringList("selected_tools"),
        executionSteps = p.toToolSteps("execution_steps"),
        reflectionHint = p.optString("reflection_hint", ""),
    )
}

fun JSONObject.toTaskInfo(): TaskInfo = TaskInfo(
    taskId = optString("task_id", ""),
    tenantId = optString("tenant_id", "default"),
    userId = optString("user_id", ""),
    prompt = optString("prompt", ""),
    state = TaskState.from(optString("state", "CREATED")),
    plan = toPlan(),
    result = optString("result", "").ifBlank { null },
    error = optString("error", "").ifBlank { null },
    modelProvider = optString("model_provider", "local"),
    modelName = optString("model_name", ""),
    createdAt = optString("created_at", ""),
    updatedAt = optString("updated_at", ""),
)

fun JSONObject.toAgentEvent(): AgentEvent = AgentEvent(
    eventType = optString("event_type", ""),
    taskId = optString("task_id", ""),
    payload = optJSONObject("payload")?.toMap() ?: emptyMap(),
    timestamp = optString("timestamp", ""),
)

fun JSONObject.toPluginInfo(): PluginInfo = PluginInfo(
    pluginId = optString("plugin_id", ""),
    version = optString("version", ""),
    toolName = optString("tool_name", ""),
    permissions = toStringList("permissions"),
    timeoutMs = optInt("timeout_ms", 4000),
    enabled = optBoolean("enabled", false),
)

fun JSONArray.toPluginList(): List<PluginInfo> =
    (0 until length()).map { getJSONObject(it).toPluginInfo() }
