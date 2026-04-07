package com.whimsical.agent.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Create
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.lifecycle.ViewModelProvider
import com.whimsical.agent.data.AgentRepository
import com.whimsical.agent.data.ApiClient
import com.whimsical.agent.data.TaskStreamClient
import com.whimsical.agent.ui.screens.AgentScreen
import com.whimsical.agent.ui.screens.CodingScreen
import com.whimsical.agent.ui.screens.MemoryScreen
import com.whimsical.agent.ui.screens.ModelScreen
import com.whimsical.agent.ui.screens.PluginScreen
import com.whimsical.agent.ui.theme.AgentTheme

class MainActivity : ComponentActivity() {

    private val baseHttp = "http://192.169.1.145:8000"
    private val baseWs = "ws://192.169.1.145:8000"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val apiClient = ApiClient(baseHttp)
        val streamClient = TaskStreamClient(baseWs)
        val repo = AgentRepository(apiClient, streamClient)

        val mainVm = ViewModelProvider(this, MainViewModel.Factory(repo))[MainViewModel::class.java]
        val memoryVm = ViewModelProvider(this, MemoryViewModel.Factory(repo))[MemoryViewModel::class.java]
        val pluginVm = ViewModelProvider(this, PluginViewModel.Factory(repo))[PluginViewModel::class.java]
        val modelVm = ViewModelProvider(this, ModelViewModel.Factory(repo))[ModelViewModel::class.java]
        val codingVm = ViewModelProvider(this, CodingViewModel.Factory(repo))[CodingViewModel::class.java]

        setContent {
            AgentTheme {
                AppShell(mainVm, memoryVm, pluginVm, modelVm, codingVm)
            }
        }
    }
}

// ── Navigation ──────────────────────────────────────────────────────────

private enum class Tab(val label: String, val icon: ImageVector) {
    AGENT("Agent", Icons.Default.Home),
    CODE("Code", Icons.Default.Create),
    MEMORY("Memory", Icons.Default.List),
    PLUGINS("Plugins", Icons.Default.Build),
    MODEL("Model", Icons.Default.Settings),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppShell(
    mainVm: MainViewModel,
    memoryVm: MemoryViewModel,
    pluginVm: PluginViewModel,
    modelVm: ModelViewModel,
    codingVm: CodingViewModel,
) {
    var selectedTab by remember { mutableStateOf(Tab.AGENT) }
    var userId by remember { mutableStateOf("demo-user") }
    val snackbarHostState = remember { SnackbarHostState() }

    Scaffold(
        topBar = { TopAppBar(title = { Text("Whimsical Agent") }) },
        snackbarHost = { SnackbarHost(snackbarHostState) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { tab ->
                    NavigationBarItem(
                        selected = selectedTab == tab,
                        onClick = { selectedTab = tab },
                        icon = { Icon(tab.icon, contentDescription = tab.label) },
                        label = { Text(tab.label) },
                    )
                }
            }
        }
    ) { padding ->
        Box(modifier = Modifier.padding(padding)) {
            when (selectedTab) {
                Tab.AGENT -> AgentScreen(mainVm)
                Tab.CODE -> CodingScreen(codingVm)
                Tab.MEMORY -> MemoryScreen(memoryVm, userId, snackbarHostState)
                Tab.PLUGINS -> PluginScreen(pluginVm)
                Tab.MODEL -> ModelScreen(modelVm)
            }
        }
    }
}
