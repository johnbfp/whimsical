package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.AgentRepository
import com.whimsical.agent.data.ModelInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class ModelViewModel(private val repo: AgentRepository) : ViewModel() {

    private val _currentModel = MutableStateFlow<ModelInfo?>(null)
    val currentModel: StateFlow<ModelInfo?> = _currentModel

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading

    private val _serverOnline = MutableStateFlow<Boolean?>(null)
    val serverOnline: StateFlow<Boolean?> = _serverOnline

    fun switchModel(provider: String, modelName: String) {
        viewModelScope.launch {
            _loading.value = true
            _error.value = null
            try {
                _currentModel.value = repo.switchModel(provider, modelName)
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun checkHealth() {
        viewModelScope.launch {
            _serverOnline.value = repo.healthCheck()
        }
    }

    class Factory(private val repo: AgentRepository) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = ModelViewModel(repo) as T
    }
}
