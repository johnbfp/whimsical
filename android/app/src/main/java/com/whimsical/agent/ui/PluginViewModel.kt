package com.whimsical.agent.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.whimsical.agent.data.AgentRepository
import com.whimsical.agent.data.PluginInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

class PluginViewModel(private val repo: AgentRepository) : ViewModel() {

    private val _plugins = MutableStateFlow<List<PluginInfo>>(emptyList())
    val plugins: StateFlow<List<PluginInfo>> = _plugins

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading

    fun refresh() {
        viewModelScope.launch {
            _loading.value = true
            try {
                _plugins.value = repo.listPlugins()
            } catch (e: Exception) {
                _error.value = e.message
            } finally {
                _loading.value = false
            }
        }
    }

    fun toggle(plugin: PluginInfo) {
        viewModelScope.launch {
            try {
                if (plugin.enabled) {
                    repo.disablePlugin(plugin.pluginId)
                } else {
                    repo.enablePlugin(plugin.pluginId)
                }
                refresh()
            } catch (e: Exception) {
                _error.value = e.message
            }
        }
    }

    class Factory(private val repo: AgentRepository) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = PluginViewModel(repo) as T
    }
}
