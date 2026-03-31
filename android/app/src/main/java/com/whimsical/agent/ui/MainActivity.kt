package com.whimsical.agent.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.data.TaskStreamClient

class MainActivity : ComponentActivity() {

    private val baseHttp = "http://10.0.2.2:8000"
    private val baseWs = "ws://10.0.2.2:8000"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val streamClient = TaskStreamClient(baseWs)
        val viewModel = MainViewModel(baseHttp, streamClient)

        setContent {
            MaterialTheme {
                AgentScreen(viewModel)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AgentScreen(viewModel: MainViewModel) {
    val events by viewModel.events.collectAsStateWithLifecycle()
    val taskId by viewModel.taskId.collectAsStateWithLifecycle()

    var userId by remember { mutableStateOf("demo-user") }
    var prompt by remember { mutableStateOf("Build me a production-grade agent runtime") }

    Scaffold(
        topBar = { TopAppBar(title = { Text("Agent Console") }) }
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .padding(16.dp)
                .fillMaxSize()
        ) {
            // User & Prompt
            OutlinedTextField(
                value = userId,
                onValueChange = { userId = it },
                label = { Text("User ID") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = prompt,
                onValueChange = { prompt = it },
                label = { Text("Prompt") },
                modifier = Modifier.fillMaxWidth(),
                minLines = 2
            )
            Spacer(Modifier.height(8.dp))

            // Action buttons
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { viewModel.createTask(userId, prompt) }) {
                    Text("Create Task")
                }
                OutlinedButton(onClick = { viewModel.cancelTask() }) {
                    Text("Cancel")
                }
            }

            // Task info
            if (taskId.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Text(
                    text = "Task: ${taskId.take(8)}…",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.Gray
                )
            }

            Spacer(Modifier.height(12.dp))
            Text("Events", style = MaterialTheme.typography.titleSmall)
            Divider(modifier = Modifier.padding(vertical = 4.dp))

            // Event list
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                items(events) { raw ->
                    Card(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 4.dp),
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)
                    ) {
                        Text(
                            text = raw,
                            modifier = Modifier.padding(8.dp),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                }
            }
        }
    }
}
