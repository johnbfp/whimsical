package com.whimsical.agent.data

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * HTTP client covering all backend REST endpoints.
 * All methods are **blocking** — call from a coroutine dispatcher (IO).
 */
class ApiClient(
    private val baseUrl: String,
    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build(),
) {
    private val json = "application/json".toMediaType()

    // ── Task ────────────────────────────────────────────────────────

    fun createTask(tenantId: String, userId: String, prompt: String): TaskInfo {
        val body = JSONObject().apply {
            put("tenant_id", tenantId)
            put("user_id", userId)
            put("prompt", prompt)
        }
        return post("/agents/tasks", body).toTaskInfo()
    }

    fun getTask(taskId: String): TaskInfo =
        get("/agents/tasks/$taskId").toTaskInfo()

    fun cancelTask(taskId: String): Boolean {
        val resp = post("/agents/tasks/$taskId/cancel", JSONObject())
        return resp.optBoolean("cancelled", false)
    }

    // ── Memory ──────────────────────────────────────────────────────

    fun recallMemory(req: MemoryRecallRequest): List<String> {
        val body = JSONObject().apply {
            put("tenant_id", req.tenantId)
            put("user_id", req.userId)
            put("query", req.query)
            put("top_k", req.topK)
        }
        val resp = post("/memory/recall", body)
        val arr = resp.optJSONArray("items") ?: return emptyList()
        return (0 until arr.length()).map { arr.getString(it) }
    }

    fun writeMemory(req: MemoryWriteRequest) {
        val body = JSONObject().apply {
            put("tenant_id", req.tenantId)
            put("user_id", req.userId)
            put("key", req.key)
            put("value", req.value)
        }
        post("/memory/write", body)
    }

    fun readMemory(tenantId: String, key: String): String? {
        val resp = get("/memory/read?tenant_id=$tenantId&key=$key")
        return resp.optString("value", "").ifBlank { null }
    }

    fun getHistory(userId: String, lastN: Int = 20): List<Map<String, Any?>> {
        val arr = getArray("/memory/history/$userId?last_n=$lastN")
        return (0 until arr.length()).map { arr.getJSONObject(it).toMap() }
    }

    fun searchMemory(tenantId: String, query: String, topK: Int = 5): List<MemoryEntry> {
        val resp = get("/memory/search?tenant_id=$tenantId&query=$query&top_k=$topK")
        val results = resp.optJSONArray("results") ?: return emptyList()
        return (0 until results.length()).map { i ->
            val o = results.getJSONObject(i)
            MemoryEntry(key = o.optString("key", ""), value = o.optString("value", ""))
        }
    }

    // ── Plugin ──────────────────────────────────────────────────────

    fun listPlugins(): List<PluginInfo> {
        val arr = getArray("/plugins")
        return arr.toPluginList()
    }

    fun enablePlugin(pluginId: String): Boolean {
        val resp = post("/plugins/$pluginId/enable", JSONObject())
        return resp.optBoolean("enabled", false)
    }

    fun disablePlugin(pluginId: String): Boolean {
        val resp = post("/plugins/$pluginId/disable", JSONObject())
        return !resp.optBoolean("enabled", true)
    }

    // ── Model ───────────────────────────────────────────────────────

    fun switchModel(provider: String, modelName: String): ModelInfo {
        val body = JSONObject().apply {
            put("provider", provider)
            put("model_name", modelName)
        }
        val resp = post("/models/switch", body)
        return ModelInfo(
            provider = resp.optString("provider", ""),
            modelName = resp.optString("model_name", ""),
        )
    }

    // ── Health ──────────────────────────────────────────────────────

    fun healthCheck(): Boolean = runCatching {
        val resp = get("/healthz")
        resp.optString("status", "") == "ok"
    }.getOrDefault(false)

    // ── Workspace (Coding) ──────────────────────────────────────────

    fun workspaceTree(path: String = ".", depth: Int = 3): JSONObject =
        get("/workspace/tree?path=${encode(path)}&depth=$depth")

    fun workspaceReadFile(path: String, startLine: Int = 0, endLine: Int = 0): JSONObject {
        var url = "/workspace/file?path=${encode(path)}"
        if (startLine > 0 && endLine > 0) url += "&start_line=$startLine&end_line=$endLine"
        return get(url)
    }

    fun workspaceWriteFile(path: String, content: String): JSONObject {
        val body = JSONObject().apply {
            put("path", path)
            put("content", content)
        }
        return post("/workspace/file", body)
    }

    fun workspaceRunCommand(command: String, timeout: Int = 30): JSONObject {
        val body = JSONObject().apply {
            put("command", command)
            put("timeout", timeout)
        }
        return post("/workspace/run", body)
    }

    fun workspaceSearch(query: String, path: String = ".", isRegex: Boolean = false, include: String = ""): JSONObject {
        var url = "/workspace/search?query=${encode(query)}&path=${encode(path)}"
        if (isRegex) url += "&is_regex=true"
        if (include.isNotBlank()) url += "&include=${encode(include)}"
        return get(url)
    }

    fun workspaceMkdir(path: String): JSONObject {
        val body = JSONObject().apply { put("path", path) }
        return post("/workspace/mkdir", body)
    }

    fun workspaceDeleteFile(path: String): JSONObject =
        delete("/workspace/file?path=${encode(path)}")

    fun workspaceRename(oldPath: String, newPath: String): JSONObject {
        val body = JSONObject().apply {
            put("old_path", oldPath)
            put("new_path", newPath)
        }
        return post("/workspace/rename", body)
    }

    fun workspaceEditFile(path: String, oldString: String, newString: String): JSONObject {
        val body = JSONObject().apply {
            put("path", path)
            put("old_string", oldString)
            put("new_string", newString)
        }
        return post("/workspace/edit", body)
    }

    fun workspaceStat(path: String): JSONObject =
        get("/workspace/stat?path=${encode(path)}")

    private fun encode(s: String): String =
        java.net.URLEncoder.encode(s, "UTF-8")

    // ── Internal ────────────────────────────────────────────────────

    private fun get(path: String): JSONObject {
        val req = Request.Builder().url("$baseUrl$path").get().build()
        client.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw ApiException(resp.code, text)
            return JSONObject(text)
        }
    }

    private fun getArray(path: String): JSONArray {
        val req = Request.Builder().url("$baseUrl$path").get().build()
        client.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw ApiException(resp.code, text)
            return JSONArray(text)
        }
    }

    private fun post(path: String, body: JSONObject): JSONObject {
        val reqBody = body.toString().toRequestBody(json)
        val req = Request.Builder().url("$baseUrl$path").post(reqBody).build()
        client.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw ApiException(resp.code, text)
            return JSONObject(text)
        }
    }

    private fun delete(path: String): JSONObject {
        val req = Request.Builder().url("$baseUrl$path").delete().build()
        client.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) throw ApiException(resp.code, text)
            return JSONObject(text)
        }
    }
}

class ApiException(val code: Int, val body: String) : Exception("HTTP $code: $body")
