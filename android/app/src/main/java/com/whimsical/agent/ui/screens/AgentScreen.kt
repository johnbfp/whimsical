package com.whimsical.agent.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.data.AgentEvent
import com.whimsical.agent.data.Plan
import com.whimsical.agent.data.TaskState
import com.whimsical.agent.ui.MainViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AgentScreen(viewModel: MainViewModel) {
    val events by viewModel.events.collectAsStateWithLifecycle()
    val taskId by viewModel.taskId.collectAsStateWithLifecycle()
    val taskState by viewModel.taskState.collectAsStateWithLifecycle()
    val taskResult by viewModel.taskResult.collectAsStateWithLifecycle()
    val currentPlan by viewModel.currentPlan.collectAsStateWithLifecycle()
    val loading by viewModel.loading.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()

    var userId by remember { mutableStateOf("demo-user") }
    var prompt by remember { mutableStateOf("Build me a production-grade agent runtime") }

    LazyColumn(
        modifier = Modifier
            .padding(16.dp)
            .fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        // User & Prompt inputs
        item {
            OutlinedTextField(
                value = userId,
                onValueChange = { userId = it },
                label = { Text("User ID") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )
        }
        item {
            OutlinedTextField(
                value = prompt,
                onValueChange = { prompt = it },
                label = { Text("Prompt") },
                modifier = Modifier.fillMaxWidth(),
                minLines = 2
            )
        }

        // Action buttons
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { viewModel.createTask(userId, prompt) },
                    enabled = !loading
                ) { Text("Create Task") }

                OutlinedButton(
                    onClick = { viewModel.cancelTask() },
                    enabled = taskId.isNotBlank() && !taskState.isTerminal
                ) { Text("Cancel") }

                IconButton(onClick = { viewModel.refreshTask() }) {
                    Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                }
            }
        }

        if (loading) {
            item { LinearProgressIndicator(modifier = Modifier.fillMaxWidth()) }
        }

        // Task info bar
        if (taskId.isNotBlank()) {
            item { TaskInfoBar(taskId, taskState) }
        }

        // Result display
        taskResult?.let {
            item {
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer)
                ) {
                    Column(modifier = Modifier.padding(12.dp)) {
                        Text("Result", style = MaterialTheme.typography.labelMedium)
                        Text(it, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        }

        // Plan display
        currentPlan?.let { plan ->
            item { PlanCard(plan) }
        }

        // Error display
        error?.let {
            item {
                Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
        }

        item {
            Text("Events (${events.size})", style = MaterialTheme.typography.titleSmall)
        }
        item { Divider() }

        // Event list — flat in the same LazyColumn, no nesting
        items(events) { event -> EventCard(event) }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TaskInfoBar(taskId: String, state: TaskState) {
    val stateColor = when (state) {
        TaskState.COMPLETED -> Color(0xFF4CAF50)
        TaskState.FAILED, TaskState.CANCELLED -> Color(0xFFF44336)
        TaskState.RUNNING, TaskState.WAITING_TOOL -> Color(0xFFFF9800)
        else -> Color.Gray
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = "Task: ${taskId.take(8)}…",
            style = MaterialTheme.typography.bodySmall,
            color = Color.Gray
        )
        Spacer(Modifier.width(12.dp))
        Badge(containerColor = stateColor) {
            Text(state.name, modifier = Modifier.padding(horizontal = 4.dp))
        }
    }
}

@Composable
private fun EventCard(event: AgentEvent) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Column(modifier = Modifier.padding(10.dp)) {
            Row {
                Text(
                    event.eventType,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary
                )
                Spacer(Modifier.weight(1f))
                Text(
                    event.timestamp.takeLast(12),
                    style = MaterialTheme.typography.labelSmall,
                    color = Color.Gray
                )
            }
            val payloadText = event.payload.entries
                .filter { it.value != null }
                .joinToString("\n") { "${it.key}: ${it.value}" }
            if (payloadText.isNotBlank()) {
                Spacer(Modifier.height(4.dp))
                Text(payloadText, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable
private fun PlanCard(plan: Plan) {
    Spacer(Modifier.height(8.dp))
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer)
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text("Execution Plan", style = MaterialTheme.typography.labelMedium)
            Spacer(Modifier.height(4.dp))

            if (plan.taskBreakdown.isNotEmpty()) {
                Text("Steps:", style = MaterialTheme.typography.labelSmall)
                plan.taskBreakdown.forEachIndexed { i, step ->
                    Text("  ${i + 1}. $step", style = MaterialTheme.typography.bodySmall)
                }
                Spacer(Modifier.height(4.dp))
            }

            if (plan.selectedTools.isNotEmpty()) {
                Text(
                    "Tools: ${plan.selectedTools.joinToString(", ")}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer
                )
            }

            if (plan.reflectionHint.isNotBlank()) {
                Spacer(Modifier.height(4.dp))
                Text(
                    "Hint: ${plan.reflectionHint}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer
                )
            }
        }
    }
}
