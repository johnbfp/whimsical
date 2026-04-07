package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.AgentRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject

// ── Data models ────────────────────────────────────────────────────────

data class FileEntry(
    val name: String,
    val path: String,
    val isDir: Boolean,
    val size: Long = 0,
    val children: List<FileEntry> = emptyList(),
)

data class FileContent(
    val path: String,
    val binary: Boolean,
    val content: String = "",
    val isImage: Boolean = false,
    val mime: String = "",
    val contentBase64: String = "",
    val language: String = "text",
    val totalLines: Int = 0,
)

data class CommandResult(
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
    val command: String = "",
    val elapsedS: Double = 0.0,
)

data class SearchMatch(
    val file: String,
    val line: Int,
    val text: String,
)

// ── ViewModel ──────────────────────────────────────────────────────────

class CodingViewModel(private val repo: AgentRepository) : ViewModel() {

    private val _fileTree = MutableStateFlow<List<FileEntry>>(emptyList())
    val fileTree: StateFlow<List<FileEntry>> = _fileTree

    private val _currentPath = MutableStateFlow(".")
    val currentPath: StateFlow<String> = _currentPath

    // Multi-tab open files
    private val _openTabs = MutableStateFlow<List<FileContent>>(emptyList())
    val openTabs: StateFlow<List<FileContent>> = _openTabs

    private val _activeTabIndex = MutableStateFlow(-1)
    val activeTabIndex: StateFlow<Int> = _activeTabIndex

    val activeFile: StateFlow<FileContent?> = MutableStateFlow(null)

    // Command history & results
    private val _commandResults = MutableStateFlow<List<CommandResult>>(emptyList())
    val commandResults: StateFlow<List<CommandResult>> = _commandResults

    private val _commandHistory = MutableStateFlow<List<String>>(emptyList())
    val commandHistory: StateFlow<List<String>> = _commandHistory

    private val _searchResults = MutableStateFlow<List<SearchMatch>>(emptyList())
    val searchResults: StateFlow<List<SearchMatch>> = _searchResults

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast

    fun clearToast() { _toast.value = null }
    fun clearError() { _error.value = null }

    private fun currentFile(): FileContent? {
        val idx = _activeTabIndex.value
        val tabs = _openTabs.value
        return if (idx in tabs.indices) tabs[idx] else null
    }

    // ── Tree ────────────────────────────────────────────────────────

    fun loadTree(path: String = ".") {
        viewModelScope.launch {
            _loading.value = true
            try {
                val json = repo.workspaceTree(path, depth = 3)
                _currentPath.value = path
                _fileTree.value = parseEntries(json.optJSONArray("entries"))
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    // ── File open (multi-tab) ───────────────────────────────────────

    fun openFile(path: String) {
        // If already open, switch to that tab
        val existing = _openTabs.value.indexOfFirst { it.path == path }
        if (existing >= 0) {
            _activeTabIndex.value = existing
            (activeFile as MutableStateFlow).value = _openTabs.value[existing]
            return
        }
        viewModelScope.launch {
            _loading.value = true
            try {
                val json = repo.workspaceReadFile(path)
                val fc = parseFileContent(json)
                _openTabs.value = _openTabs.value + fc
                _activeTabIndex.value = _openTabs.value.size - 1
                (activeFile as MutableStateFlow).value = fc
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun switchTab(index: Int) {
        if (index in _openTabs.value.indices) {
            _activeTabIndex.value = index
            (activeFile as MutableStateFlow).value = _openTabs.value[index]
        }
    }

    fun closeTab(index: Int) {
        val tabs = _openTabs.value.toMutableList()
        if (index !in tabs.indices) return
        tabs.removeAt(index)
        _openTabs.value = tabs
        val newIdx = when {
            tabs.isEmpty() -> -1
            index >= tabs.size -> tabs.size - 1
            else -> index
        }
        _activeTabIndex.value = newIdx
        (activeFile as MutableStateFlow).value = if (newIdx >= 0) tabs[newIdx] else null
    }

    fun closeAllTabs() {
        _openTabs.value = emptyList()
        _activeTabIndex.value = -1
        (activeFile as MutableStateFlow).value = null
    }

    // ── File write ──────────────────────────────────────────────────

    fun saveFile(path: String, content: String) {
        viewModelScope.launch {
            _loading.value = true
            try {
                repo.workspaceWriteFile(path, content)
                _toast.value = "✓ $path saved"
                // Update the tab content in place
                val tabs = _openTabs.value.toMutableList()
                val idx = tabs.indexOfFirst { it.path == path }
                if (idx >= 0) {
                    tabs[idx] = tabs[idx].copy(content = content, totalLines = content.count { it == '\n' } + 1)
                    _openTabs.value = tabs
                    if (_activeTabIndex.value == idx) {
                        (activeFile as MutableStateFlow).value = tabs[idx]
                    }
                }
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    // ── File operations ─────────────────────────────────────────────

    fun createNewFile(path: String, content: String = "") {
        viewModelScope.launch {
            _loading.value = true
            try {
                repo.workspaceWriteFile(path, content)
                _toast.value = "✓ Created $path"
                loadTree(_currentPath.value)
                if (content.isNotEmpty()) openFile(path)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun createDir(path: String) {
        viewModelScope.launch {
            _loading.value = true
            try {
                repo.workspaceMkdir(path)
                _toast.value = "✓ Created folder $path"
                loadTree(_currentPath.value)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun deleteFile(path: String) {
        viewModelScope.launch {
            _loading.value = true
            try {
                repo.workspaceDeleteFile(path)
                _toast.value = "✓ Deleted $path"
                // Close tab if open
                val idx = _openTabs.value.indexOfFirst { it.path == path }
                if (idx >= 0) closeTab(idx)
                loadTree(_currentPath.value)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun renameFile(oldPath: String, newPath: String) {
        viewModelScope.launch {
            _loading.value = true
            try {
                repo.workspaceRename(oldPath, newPath)
                _toast.value = "✓ Renamed → $newPath"
                loadTree(_currentPath.value)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    // ── Command (with history) ──────────────────────────────────────

    fun runCommand(command: String, timeout: Int = 30) {
        if (command.isBlank()) return
        // Add to history (dedup)
        val hist = _commandHistory.value.toMutableList()
        hist.remove(command)
        hist.add(0, command)
        if (hist.size > 50) hist.removeAt(hist.size - 1)
        _commandHistory.value = hist

        viewModelScope.launch {
            _loading.value = true
            try {
                val json = repo.workspaceRunCommand(command, timeout)
                val result = CommandResult(
                    exitCode = json.optInt("exit_code", -1),
                    stdout = json.optString("stdout", ""),
                    stderr = json.optString("stderr", ""),
                    command = command,
                    elapsedS = json.optDouble("elapsed_s", 0.0),
                )
                _commandResults.value = listOf(result) + _commandResults.value.take(49)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    // ── Search ──────────────────────────────────────────────────────

    fun search(query: String, path: String = ".", isRegex: Boolean = false, include: String = "") {
        viewModelScope.launch {
            _loading.value = true
            try {
                val json = repo.workspaceSearch(query, path, isRegex, include)
                val arr = json.optJSONArray("matches")
                val list = mutableListOf<SearchMatch>()
                if (arr != null) {
                    for (i in 0 until arr.length()) {
                        val o = arr.getJSONObject(i)
                        list.add(SearchMatch(
                            file = o.optString("file", ""),
                            line = o.optInt("line", 0),
                            text = o.optString("text", ""),
                        ))
                    }
                }
                _searchResults.value = list
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    // ── JSON helpers ────────────────────────────────────────────────

    private fun parseEntries(arr: org.json.JSONArray?): List<FileEntry> {
        if (arr == null) return emptyList()
        return (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            FileEntry(
                name = o.optString("name", ""),
                path = o.optString("path", ""),
                isDir = o.optBoolean("is_dir", false),
                size = o.optLong("size", 0),
                children = parseEntries(o.optJSONArray("children")),
            )
        }
    }

    private fun parseFileContent(json: JSONObject): FileContent = FileContent(
        path = json.optString("path", ""),
        binary = json.optBoolean("binary", false),
        content = json.optString("content", ""),
        isImage = json.optBoolean("is_image", false),
        mime = json.optString("mime", ""),
        contentBase64 = json.optString("content_base64", ""),
        language = json.optString("language", "text"),
        totalLines = json.optInt("total_lines", 0),
    )

    // ── Factory ─────────────────────────────────────────────────────

    class Factory(private val repo: AgentRepository) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = CodingViewModel(repo) as T
    }
}
