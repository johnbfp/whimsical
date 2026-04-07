package com.whimsical.agent.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.ui.ModelViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModelScreen(viewModel: ModelViewModel) {
    val currentModel by viewModel.currentModel.collectAsStateWithLifecycle()
    val loading by viewModel.loading.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()
    val serverOnline by viewModel.serverOnline.collectAsStateWithLifecycle()

    var provider by remember { mutableStateOf("ollama") }
    var modelName by remember { mutableStateOf("qwen2.5:7b") }

    LaunchedEffect(Unit) { viewModel.checkHealth() }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("Model Gateway", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))

        // Health indicator
        Row(verticalAlignment = Alignment.CenterVertically) {
            val (color, label) = when (serverOnline) {
                true -> Color(0xFF4CAF50) to "Server online"
                false -> Color(0xFFF44336) to "Server offline"
                null -> Color.Gray to "Checking…"
            }
            Badge(containerColor = color) { Text(" ") }
            Spacer(Modifier.width(8.dp))
            Text(label, style = MaterialTheme.typography.bodySmall)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { viewModel.checkHealth() }) { Text("Refresh") }
        }

        Spacer(Modifier.height(16.dp))

        // Current model display
        currentModel?.let {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(12.dp)) {
                    Text("Active Model", style = MaterialTheme.typography.labelMedium)
                    Text("${it.provider} / ${it.modelName}", style = MaterialTheme.typography.bodyLarge)
                }
            }
            Spacer(Modifier.height(16.dp))
        }

        // Provider selector
        Text("Provider", style = MaterialTheme.typography.labelMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("ollama", "openai", "anthropic").forEach { p ->
                FilterChip(
                    selected = provider == p,
                    onClick = { provider = p },
                    label = { Text(p) }
                )
            }
        }

        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = modelName,
            onValueChange = { modelName = it },
            label = { Text("Model name") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true
        )
        Spacer(Modifier.height(12.dp))
        Button(
            onClick = { viewModel.switchModel(provider, modelName) },
            enabled = modelName.isNotBlank() && !loading,
        ) { Text("Switch Model") }

        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth().padding(top = 8.dp))

        error?.let {
            Spacer(Modifier.height(8.dp))
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
    }
}
