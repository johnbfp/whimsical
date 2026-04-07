package com.whimsical.agent.data

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ModelsTest {

    // ── TaskState ───────────────────────────────────────────────────

    @Test
    fun `TaskState from valid name`() {
        assertEquals(TaskState.COMPLETED, TaskState.from("COMPLETED"))
        assertEquals(TaskState.RUNNING, TaskState.from("running"))
    }

    @Test
    fun `TaskState from invalid falls back to CREATED`() {
        assertEquals(TaskState.CREATED, TaskState.from("UNKNOWN"))
    }

    @Test
    fun `isTerminal returns true for terminal states`() {
        assertTrue(TaskState.COMPLETED.isTerminal)
        assertTrue(TaskState.FAILED.isTerminal)
        assertTrue(TaskState.CANCELLED.isTerminal)
        assertFalse(TaskState.RUNNING.isTerminal)
        assertFalse(TaskState.PLANNING.isTerminal)
    }

    // ── JSON → TaskInfo ─────────────────────────────────────────────

    @Test
    fun `toTaskInfo parses minimal JSON`() {
        val json = JSONObject().apply {
            put("task_id", "abc-123")
            put("tenant_id", "t1")
            put("user_id", "u1")
            put("prompt", "hello world")
            put("state", "RUNNING")
        }
        val task = json.toTaskInfo()
        assertEquals("abc-123", task.taskId)
        assertEquals("t1", task.tenantId)
        assertEquals(TaskState.RUNNING, task.state)
        assertNull(task.plan)
        assertNull(task.result)
    }

    @Test
    fun `toTaskInfo parses full JSON with plan`() {
        val json = JSONObject("""
        {
            "task_id": "t-1",
            "tenant_id": "default",
            "user_id": "u1",
            "prompt": "test",
            "state": "COMPLETED",
            "result": "done",
            "model_provider": "ollama",
            "model_name": "llama3",
            "plan": {
                "task_breakdown": ["step1", "step2"],
                "selected_tools": ["echo"],
                "execution_steps": [
                    {"tool_name": "echo", "input": {"text": "hi"}}
                ],
                "reflection_hint": "looks good"
            }
        }
        """.trimIndent())
        val task = json.toTaskInfo()
        assertEquals(TaskState.COMPLETED, task.state)
        assertEquals("done", task.result)
        assertNotNull(task.plan)
        assertEquals(2, task.plan!!.taskBreakdown.size)
        assertEquals("echo", task.plan!!.executionSteps[0].toolName)
        assertEquals("looks good", task.plan!!.reflectionHint)
    }

    // ── JSON → AgentEvent ───────────────────────────────────────────

    @Test
    fun `toAgentEvent parses event`() {
        val json = JSONObject("""
        {
            "event_type": "task_event",
            "task_id": "t-1",
            "payload": {"state": "RUNNING", "detail": "planning started"},
            "timestamp": "2025-01-01T00:00:00Z"
        }
        """.trimIndent())
        val event = json.toAgentEvent()
        assertEquals("task_event", event.eventType)
        assertEquals("t-1", event.taskId)
        assertEquals("RUNNING", event.payload["state"])
    }

    // ── JSON → PluginInfo ───────────────────────────────────────────

    @Test
    fun `toPluginInfo parses plugin`() {
        val json = JSONObject("""
        {
            "plugin_id": "builtin.echo",
            "version": "1.0.0",
            "tool_name": "echo",
            "permissions": ["tool.execute"],
            "timeout_ms": 1000,
            "enabled": true
        }
        """.trimIndent())
        val plugin = json.toPluginInfo()
        assertEquals("builtin.echo", plugin.pluginId)
        assertTrue(plugin.enabled)
        assertEquals(1000, plugin.timeoutMs)
        assertEquals(listOf("tool.execute"), plugin.permissions)
    }

    @Test
    fun `toPluginList parses array`() {
        val arr = JSONArray().apply {
            put(JSONObject().apply {
                put("plugin_id", "p1"); put("version", "1.0.0")
                put("tool_name", "t1"); put("permissions", JSONArray(listOf("x")))
                put("enabled", false)
            })
            put(JSONObject().apply {
                put("plugin_id", "p2"); put("version", "2.0.0")
                put("tool_name", "t2"); put("permissions", JSONArray(listOf("y")))
                put("enabled", true)
            })
        }
        val list = arr.toPluginList()
        assertEquals(2, list.size)
        assertEquals("p1", list[0].pluginId)
        assertTrue(list[1].enabled)
    }

    // ── JSONObject.toMap / JSONArray.toList ──────────────────────────

    @Test
    fun `toMap handles nested objects and null`() {
        val json = JSONObject("""
        {
            "a": 1,
            "b": "text",
            "c": null,
            "d": {"nested": true},
            "e": [1, 2, 3]
        }
        """.trimIndent())
        val map = json.toMap()
        assertEquals(1, map["a"])
        assertEquals("text", map["b"])
        assertNull(map["c"])
        @Suppress("UNCHECKED_CAST")
        assertTrue((map["d"] as Map<String, Any?>)["nested"] as Boolean)
        assertEquals(listOf(1, 2, 3), map["e"])
    }
}
