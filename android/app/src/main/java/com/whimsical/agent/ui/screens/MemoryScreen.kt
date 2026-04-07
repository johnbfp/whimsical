package com.whimsical.agent.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.ui.MemoryViewModel
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MemoryScreen(
    viewModel: MemoryViewModel,
    userId: String,
    snackbarHostState: SnackbarHostState = remember { SnackbarHostState() },
) {
    val recallResults by viewModel.recallResults.collectAsStateWithLifecycle()
    val searchResults by viewModel.searchResults.collectAsStateWithLifecycle()
    val history by viewModel.history.collectAsStateWithLifecycle()
    val loading by viewModel.loading.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()
    val writeSuccess by viewModel.writeSuccess.collectAsStateWithLifecycle()

    val scope = rememberCoroutineScope()

    // Show snackbar on write success
    LaunchedEffect(writeSuccess) {
        if (writeSuccess) {
            scope.launch { snackbarHostState.showSnackbar("Memory saved successfully") }
            viewModel.clearWriteSuccess()
        }
    }

    var selectedTab by remember { mutableIntStateOf(0) }
    val tabs = listOf("Recall", "Write", "Search", "History")

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        TabRow(selectedTabIndex = selectedTab) {
            tabs.forEachIndexed { index, title ->
                Tab(selected = selectedTab == index, onClick = { selectedTab = index }) {
                    Text(title, modifier = Modifier.padding(12.dp))
                }
            }
        }

        Spacer(Modifier.height(12.dp))

        when (selectedTab) {
            0 -> RecallTab(viewModel, userId, recallResults, loading)
            1 -> WriteTab(viewModel, userId, loading)
            2 -> SearchTab(viewModel, searchResults.map { "${it.key}: ${it.value}" }, loading)
            3 -> HistoryTab(viewModel, userId, history, loading)
        }

        error?.let {
            Spacer(Modifier.height(8.dp))
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun RecallTab(vm: MemoryViewModel, userId: String, results: List<String>, loading: Boolean) {
    var query by remember { mutableStateOf("") }

    Column(modifier = Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            label = { Text("Query") },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            trailingIcon = {
                IconButton(onClick = { vm.recall(userId, query) }) {
                    Icon(Icons.Default.Search, contentDescription = "Recall")
                }
            }
        )
        Spacer(Modifier.height(8.dp))
        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        LazyColumn(modifier = Modifier.weight(1f)) {
            items(results) { item ->
                Card(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                    Text(item, modifier = Modifier.padding(10.dp), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
private fun WriteTab(vm: MemoryViewModel, userId: String, loading: Boolean) {
    var key by remember { mutableStateOf("") }
    var value by remember { mutableStateOf("") }

    OutlinedTextField(
        value = key, onValueChange = { key = it },
        label = { Text("Key") }, modifier = Modifier.fillMaxWidth(), singleLine = true
    )
    Spacer(Modifier.height(8.dp))
    OutlinedTextField(
        value = value, onValueChange = { value = it },
        label = { Text("Value") }, modifier = Modifier.fillMaxWidth(), minLines = 3
    )
    Spacer(Modifier.height(8.dp))
    Button(
        onClick = { vm.write(userId, key, value) },
        enabled = key.isNotBlank() && value.isNotBlank() && !loading
    ) { Text("Save") }
    if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth().padding(top = 8.dp))
}

@Composable
private fun SearchTab(vm: MemoryViewModel, results: List<String>, loading: Boolean) {
    var query by remember { mutableStateOf("") }

    Column(modifier = Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = query, onValueChange = { query = it },
            label = { Text("Search query") }, modifier = Modifier.fillMaxWidth(), singleLine = true,
            trailingIcon = {
                IconButton(onClick = { vm.search(query) }) {
                    Icon(Icons.Default.Search, contentDescription = "Search")
                }
            }
        )
        Spacer(Modifier.height(8.dp))
        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        LazyColumn(modifier = Modifier.weight(1f)) {
            items(results) { item ->
                Card(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                    Text(item, modifier = Modifier.padding(10.dp), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
private fun HistoryTab(
    vm: MemoryViewModel,
    userId: String,
    history: List<Map<String, Any?>>,
    loading: Boolean,
) {
    LaunchedEffect(userId) { vm.loadHistory(userId) }

    Column(modifier = Modifier.fillMaxSize()) {
        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        LazyColumn(modifier = Modifier.weight(1f)) {
            items(history) { entry ->
                Card(modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                    Text(
                        entry.entries.joinToString("\n") { "${it.key}: ${it.value}" },
                        modifier = Modifier.padding(10.dp),
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
        }
    }
}
