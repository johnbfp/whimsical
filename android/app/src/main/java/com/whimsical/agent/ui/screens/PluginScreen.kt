package com.whimsical.agent.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.data.PluginInfo
import com.whimsical.agent.ui.PluginViewModel

@Composable
fun PluginScreen(viewModel: PluginViewModel) {
    val plugins by viewModel.plugins.collectAsStateWithLifecycle()
    val loading by viewModel.loading.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { viewModel.refresh() }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Plugins", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { viewModel.refresh() }) { Text("Refresh") }
        }

        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp))

        error?.let {
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            Spacer(Modifier.height(4.dp))
        }

        LazyColumn {
            items(plugins, key = { it.pluginId }) { plugin ->
                PluginCard(plugin, onToggle = { viewModel.toggle(plugin) })
            }
        }
    }
}

@Composable
private fun PluginCard(plugin: PluginInfo, onToggle: () -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        colors = CardDefaults.cardColors(
            containerColor = if (plugin.enabled) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically)
        {
            Column(modifier = Modifier.weight(1f)) {
                Text(plugin.pluginId, style = MaterialTheme.typography.bodyMedium)
                Text(
                    "tool: ${plugin.toolName}  v${plugin.version}  timeout: ${plugin.timeoutMs}ms",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Switch(checked = plugin.enabled, onCheckedChange = { onToggle() })
        }
    }
}
