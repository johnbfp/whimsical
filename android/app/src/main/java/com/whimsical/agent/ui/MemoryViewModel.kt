package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.AgentRepository
import com.whimsical.agent.data.MemoryEntry
import com.whimsical.agent.data.MemoryRecallRequest
import com.whimsical.agent.data.MemoryWriteRequest
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class MemoryViewModel(private val repo: AgentRepository) : ViewModel() {

    private val _recallResults = MutableStateFlow<List<String>>(emptyList())
    val recallResults: StateFlow<List<String>> = _recallResults

    private val _searchResults = MutableStateFlow<List<MemoryEntry>>(emptyList())
    val searchResults: StateFlow<List<MemoryEntry>> = _searchResults

    private val _history = MutableStateFlow<List<Map<String, Any?>>>(emptyList())
    val history: StateFlow<List<Map<String, Any?>>> = _history

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading

    private val _writeSuccess = MutableStateFlow(false)
    val writeSuccess: StateFlow<Boolean> = _writeSuccess

    fun clearWriteSuccess() { _writeSuccess.value = false }

    fun recall(userId: String, query: String, topK: Int = 5) {
        viewModelScope.launch {
            _loading.value = true
            try {
                _recallResults.value = repo.recallMemory(
                    MemoryRecallRequest(userId = userId, query = query, topK = topK)
                )
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun write(userId: String, key: String, value: String) {
        viewModelScope.launch {
            _loading.value = true
            _writeSuccess.value = false
            try {
                repo.writeMemory(MemoryWriteRequest(userId = userId, key = key, value = value))
                _writeSuccess.value = true
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun search(query: String, topK: Int = 5) {
        viewModelScope.launch {
            _loading.value = true
            try {
                _searchResults.value = repo.searchMemory("default", query, topK)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun loadHistory(userId: String) {
        viewModelScope.launch {
            _loading.value = true
            try {
                _history.value = repo.getHistory(userId)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    class Factory(private val repo: AgentRepository) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = MemoryViewModel(repo) as T
    }
}
