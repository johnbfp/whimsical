package com.whimsical.agent.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Single entry-point that wraps [ApiClient] + [TaskStreamClient].
 * ViewModel层通过此 Repository 访问所有数据，隔离网络细节。
 */
class AgentRepository(
    private val api: ApiClient,
    val stream: TaskStreamClient,
) {
    // ── Task ────────────────────────────────────────────────────────

    suspend fun createTask(tenantId: String, userId: String, prompt: String): TaskInfo =
        withContext(Dispatchers.IO) { api.createTask(tenantId, userId, prompt) }

    suspend fun getTask(taskId: String): TaskInfo =
        withContext(Dispatchers.IO) { api.getTask(taskId) }

    suspend fun cancelTask(taskId: String): Boolean =
        withContext(Dispatchers.IO) { api.cancelTask(taskId) }

    fun subscribeEvents(taskId: String) = stream.subscribe(taskId)

    fun closeStream() = stream.close()

    // ── Memory ──────────────────────────────────────────────────────

    suspend fun recallMemory(req: MemoryRecallRequest): List<String> =
        withContext(Dispatchers.IO) { api.recallMemory(req) }

    suspend fun writeMemory(req: MemoryWriteRequest) =
        withContext(Dispatchers.IO) { api.writeMemory(req) }

    suspend fun readMemory(tenantId: String, key: String): String? =
        withContext(Dispatchers.IO) { api.readMemory(tenantId, key) }

    suspend fun getHistory(userId: String, lastN: Int = 20): List<Map<String, Any?>> =
        withContext(Dispatchers.IO) { api.getHistory(userId, lastN) }

    suspend fun searchMemory(tenantId: String, query: String, topK: Int = 5): List<MemoryEntry> =
        withContext(Dispatchers.IO) { api.searchMemory(tenantId, query, topK) }

    // ── Plugin ──────────────────────────────────────────────────────

    suspend fun listPlugins(): List<PluginInfo> =
        withContext(Dispatchers.IO) { api.listPlugins() }

    suspend fun enablePlugin(pluginId: String): Boolean =
        withContext(Dispatchers.IO) { api.enablePlugin(pluginId) }

    suspend fun disablePlugin(pluginId: String): Boolean =
        withContext(Dispatchers.IO) { api.disablePlugin(pluginId) }

    // ── Model ───────────────────────────────────────────────────────

    suspend fun switchModel(provider: String, modelName: String): ModelInfo =
        withContext(Dispatchers.IO) { api.switchModel(provider, modelName) }

    // ── Health ──────────────────────────────────────────────────────

    suspend fun healthCheck(): Boolean =
        withContext(Dispatchers.IO) { api.healthCheck() }

    // ── Workspace (Coding) ──────────────────────────────────────────

    suspend fun workspaceTree(path: String = ".", depth: Int = 3) =
        withContext(Dispatchers.IO) { api.workspaceTree(path, depth) }

    suspend fun workspaceReadFile(path: String, startLine: Int = 0, endLine: Int = 0) =
        withContext(Dispatchers.IO) { api.workspaceReadFile(path, startLine, endLine) }

    suspend fun workspaceWriteFile(path: String, content: String) =
        withContext(Dispatchers.IO) { api.workspaceWriteFile(path, content) }

    suspend fun workspaceRunCommand(command: String, timeout: Int = 30) =
        withContext(Dispatchers.IO) { api.workspaceRunCommand(command, timeout) }

    suspend fun workspaceSearch(query: String, path: String = ".", isRegex: Boolean = false, include: String = "") =
        withContext(Dispatchers.IO) { api.workspaceSearch(query, path, isRegex, include) }

    suspend fun workspaceMkdir(path: String) =
        withContext(Dispatchers.IO) { api.workspaceMkdir(path) }

    suspend fun workspaceDeleteFile(path: String) =
        withContext(Dispatchers.IO) { api.workspaceDeleteFile(path) }

    suspend fun workspaceRename(oldPath: String, newPath: String) =
        withContext(Dispatchers.IO) { api.workspaceRename(oldPath, newPath) }

    suspend fun workspaceEditFile(path: String, oldString: String, newString: String) =
        withContext(Dispatchers.IO) { api.workspaceEditFile(path, oldString, newString) }

    suspend fun workspaceStat(path: String) =
        withContext(Dispatchers.IO) { api.workspaceStat(path) }
}
