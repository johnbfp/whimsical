package com.whimsical.agent.ui.screens

import android.graphics.BitmapFactory
import android.util.Base64
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Create
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.whimsical.agent.ui.CodingViewModel
import com.whimsical.agent.ui.CommandResult
import com.whimsical.agent.ui.FileContent
import com.whimsical.agent.ui.FileEntry
import com.whimsical.agent.ui.SearchMatch

// ── Syntax highlight colors ────────────────────────────────────────────

private val kwColor = Color(0xFFCC7832)      // keywords
private val strColor = Color(0xFF6A8759)     // strings
private val commentColor = Color(0xFF808080) // comments
private val numColor = Color(0xFF6897BB)     // numbers

private val KEYWORDS = setOf(
    "def", "class", "import", "from", "return", "if", "else", "elif", "for",
    "while", "try", "except", "finally", "with", "as", "yield", "raise",
    "pass", "break", "continue", "and", "or", "not", "in", "is", "None",
    "True", "False", "lambda", "async", "await",
    "fun", "val", "var", "when", "object", "companion", "data", "sealed",
    "override", "suspend", "private", "public", "protected", "internal",
    "abstract", "interface", "enum", "const", "this", "super",
    "function", "let", "export", "default", "typeof",
    "instanceof", "new", "delete", "void", "throw", "catch", "switch",
    "case", "extends", "implements", "static",
    "fn", "mut", "pub", "use", "mod", "struct", "impl", "trait",
    "match", "loop", "move", "ref", "self", "Self", "crate", "where",
)

// ── Tab modes ──────────────────────────────────────────────────────────

private enum class CodingTab { FILES, EDITOR, TERMINAL, SEARCH }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CodingScreen(viewModel: CodingViewModel) {
    val fileTree by viewModel.fileTree.collectAsStateWithLifecycle()
    val openTabs by viewModel.openTabs.collectAsStateWithLifecycle()
    val activeTabIndex by viewModel.activeTabIndex.collectAsStateWithLifecycle()
    val activeFile by viewModel.activeFile.collectAsStateWithLifecycle()
    val commandResults by viewModel.commandResults.collectAsStateWithLifecycle()
    val commandHistory by viewModel.commandHistory.collectAsStateWithLifecycle()
    val searchResults by viewModel.searchResults.collectAsStateWithLifecycle()
    val loading by viewModel.loading.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()
    val toast by viewModel.toast.collectAsStateWithLifecycle()
    val currentPath by viewModel.currentPath.collectAsStateWithLifecycle()

    var activeSection by remember { mutableStateOf(CodingTab.FILES) }

    LaunchedEffect(activeFile) {
        if (activeFile != null && activeSection == CodingTab.FILES) {
            activeSection = CodingTab.EDITOR
        }
    }

    LaunchedEffect(Unit) { viewModel.loadTree() }

    LaunchedEffect(toast) {
        if (toast != null) {
            kotlinx.coroutines.delay(2000)
            viewModel.clearToast()
        }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Tab row
        ScrollableTabRow(
            selectedTabIndex = activeSection.ordinal,
            edgePadding = 0.dp,
        ) {
            CodingTab.entries.forEach { tab ->
                val label = when (tab) {
                    CodingTab.EDITOR -> "Editor(${openTabs.size})"
                    else -> tab.name.lowercase().replaceFirstChar { it.uppercase() }
                }
                Tab(
                    selected = activeSection == tab,
                    onClick = { activeSection = tab },
                    text = { Text(label, maxLines = 1, fontSize = 13.sp) },
                )
            }
        }

        // Status bar
        if (loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        if (error != null) {
            Text(
                error ?: "",
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0x20FF0000))
                    .padding(horizontal = 12.dp, vertical = 4.dp)
                    .clickable { viewModel.clearError() },
                fontSize = 12.sp, maxLines = 2,
            )
        }
        if (toast != null) {
            Text(
                toast ?: "",
                color = Color(0xFF388E3C),
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 2.dp),
                fontSize = 12.sp,
            )
        }

        // Content
        when (activeSection) {
            CodingTab.FILES -> FileBrowser(
                entries = fileTree,
                currentPath = currentPath,
                onNavigate = { viewModel.loadTree(it) },
                onOpenFile = { viewModel.openFile(it) },
                onRefresh = { viewModel.loadTree(currentPath) },
                onNewFile = { path, content -> viewModel.createNewFile(path, content) },
                onNewFolder = { viewModel.createDir(it) },
                onDelete = { viewModel.deleteFile(it) },
                onRename = { old, new -> viewModel.renameFile(old, new) },
            )
            CodingTab.EDITOR -> EditorPanel(
                tabs = openTabs,
                activeIndex = activeTabIndex,
                activeFile = activeFile,
                onSwitchTab = { viewModel.switchTab(it) },
                onCloseTab = { viewModel.closeTab(it) },
                onSave = { path, content -> viewModel.saveFile(path, content) },
            )
            CodingTab.TERMINAL -> TerminalPanel(
                results = commandResults,
                history = commandHistory,
                onRun = { cmd, timeout -> viewModel.runCommand(cmd, timeout) },
            )
            CodingTab.SEARCH -> SearchPanel(
                results = searchResults,
                onSearch = { q, regex, include -> viewModel.search(q, ".", regex, include) },
                onOpenMatch = { match ->
                    viewModel.openFile(match.file)
                    activeSection = CodingTab.EDITOR
                },
            )
        }
    }
}

// ── File Browser ───────────────────────────────────────────────────────

@Composable
private fun FileBrowser(
    entries: List<FileEntry>,
    currentPath: String,
    onNavigate: (String) -> Unit,
    onOpenFile: (String) -> Unit,
    onRefresh: () -> Unit,
    onNewFile: (String, String) -> Unit,
    onNewFolder: (String) -> Unit,
    onDelete: (String) -> Unit,
    onRename: (String, String) -> Unit,
) {
    var showNewDialog by remember { mutableStateOf(false) }
    var newIsFolder by remember { mutableStateOf(false) }
    var newName by remember { mutableStateOf("") }
    var renameTarget by remember { mutableStateOf<FileEntry?>(null) }
    var renameName by remember { mutableStateOf("") }
    var deleteTarget by remember { mutableStateOf<FileEntry?>(null) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (currentPath != ".") {
                IconButton(onClick = {
                    val parent = currentPath.substringBeforeLast("/", ".")
                    onNavigate(parent)
                }, modifier = Modifier.size(32.dp)) {
                    Icon(Icons.Default.ArrowBack, "Up", modifier = Modifier.size(18.dp))
                }
            } else {
                IconButton(onClick = { onNavigate(".") }, modifier = Modifier.size(32.dp)) {
                    Icon(Icons.Default.Home, "Root", modifier = Modifier.size(18.dp))
                }
            }
            Text(
                if (currentPath == ".") "/" else "/$currentPath",
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
            IconButton(onClick = { newIsFolder = false; newName = ""; showNewDialog = true }, modifier = Modifier.size(32.dp)) {
                Icon(Icons.Default.Add, "New File", modifier = Modifier.size(18.dp))
            }
            IconButton(onClick = { newIsFolder = true; newName = ""; showNewDialog = true }, modifier = Modifier.size(32.dp)) {
                Icon(Icons.Default.List, "New Folder", modifier = Modifier.size(18.dp))
            }
            IconButton(onClick = onRefresh, modifier = Modifier.size(32.dp)) {
                Icon(Icons.Default.Refresh, "Refresh", modifier = Modifier.size(18.dp))
            }
        }
        Divider()

        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(entries) { entry ->
                FileRow(
                    entry = entry,
                    onNavigate = onNavigate,
                    onOpenFile = onOpenFile,
                    onDelete = { deleteTarget = entry },
                    onRename = { renameTarget = entry; renameName = entry.name },
                )
            }
        }
    }

    if (showNewDialog) {
        AlertDialog(
            onDismissRequest = { showNewDialog = false },
            title = { Text(if (newIsFolder) "New Folder" else "New File") },
            text = {
                OutlinedTextField(
                    value = newName,
                    onValueChange = { newName = it },
                    label = { Text("Name") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    val fullPath = if (currentPath == ".") newName else "$currentPath/$newName"
                    if (newIsFolder) onNewFolder(fullPath) else onNewFile(fullPath, "")
                    showNewDialog = false
                }) { Text("Create") }
            },
            dismissButton = { TextButton(onClick = { showNewDialog = false }) { Text("Cancel") } },
        )
    }

    if (renameTarget != null) {
        AlertDialog(
            onDismissRequest = { renameTarget = null },
            title = { Text("Rename") },
            text = {
                OutlinedTextField(
                    value = renameName,
                    onValueChange = { renameName = it },
                    label = { Text("New name") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    val oldPath = renameTarget!!.path
                    val parentDir = oldPath.substringBeforeLast("/", "")
                    val newPath = if (parentDir.isEmpty()) renameName else "$parentDir/$renameName"
                    onRename(oldPath, newPath)
                    renameTarget = null
                }) { Text("Rename") }
            },
            dismissButton = { TextButton(onClick = { renameTarget = null }) { Text("Cancel") } },
        )
    }

    if (deleteTarget != null) {
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text("Delete?") },
            text = { Text("Delete ${deleteTarget!!.name}?") },
            confirmButton = {
                TextButton(onClick = {
                    onDelete(deleteTarget!!.path)
                    deleteTarget = null
                }) { Text("Delete", color = Color.Red) }
            },
            dismissButton = { TextButton(onClick = { deleteTarget = null }) { Text("Cancel") } },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FileRow(
    entry: FileEntry,
    onNavigate: (String) -> Unit,
    onOpenFile: (String) -> Unit,
    onDelete: () -> Unit,
    onRename: () -> Unit,
) {
    var showMenu by remember { mutableStateOf(false) }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable {
                if (entry.isDir) onNavigate(entry.path)
                else onOpenFile(entry.path)
            }
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (entry.isDir) "\uD83D\uDCC1" else fileIcon(entry.name), fontSize = 16.sp)
        Spacer(modifier = Modifier.width(8.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                entry.name,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = if (entry.isDir) FontWeight.SemiBold else FontWeight.Normal,
                maxLines = 1,
            )
            if (!entry.isDir && entry.size > 0) {
                Text(
                    formatSize(entry.size),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Box {
            IconButton(onClick = { showMenu = true }, modifier = Modifier.size(28.dp)) {
                Text("\u22EE", fontSize = 16.sp)
            }
            DropdownMenu(expanded = showMenu, onDismissRequest = { showMenu = false }) {
                DropdownMenuItem(text = { Text("Rename") }, onClick = { showMenu = false; onRename() })
                DropdownMenuItem(text = { Text("Delete", color = Color.Red) }, onClick = { showMenu = false; onDelete() })
            }
        }
    }
}

// ── Editor Panel (multi-tab) ───────────────────────────────────────────

@Composable
private fun EditorPanel(
    tabs: List<FileContent>,
    activeIndex: Int,
    activeFile: FileContent?,
    onSwitchTab: (Int) -> Unit,
    onCloseTab: (Int) -> Unit,
    onSave: (String, String) -> Unit,
) {
    if (tabs.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("No files open\nTap a file in Files tab to open", color = Color.Gray, fontSize = 14.sp)
        }
        return
    }

    Column(modifier = Modifier.fillMaxSize()) {
        ScrollableTabRow(
            selectedTabIndex = if (activeIndex in tabs.indices) activeIndex else 0,
            edgePadding = 0.dp,
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ) {
            tabs.forEachIndexed { i, fc ->
                Tab(
                    selected = i == activeIndex,
                    onClick = { onSwitchTab(i) },
                    text = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                fc.path.substringAfterLast("/"),
                                fontSize = 12.sp, maxLines = 1,
                                modifier = Modifier.widthIn(max = 120.dp),
                            )
                            Spacer(modifier = Modifier.width(4.dp))
                            Icon(
                                Icons.Default.Close, "Close",
                                modifier = Modifier
                                    .size(14.dp)
                                    .clickable { onCloseTab(i) },
                            )
                        }
                    },
                )
            }
        }

        if (activeFile != null) {
            FileViewer(file = activeFile, onSave = onSave)
        }
    }
}

// ── File Viewer / Editor ───────────────────────────────────────────────

@Composable
private fun FileViewer(
    file: FileContent,
    onSave: (String, String) -> Unit,
) {
    var editing by remember(file.path) { mutableStateOf(false) }
    var editText by remember(file.path) { mutableStateOf(file.content) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surfaceVariant)
                .padding(horizontal = 8.dp, vertical = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                file.path,
                style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
            Text(
                "${file.language} \u2022 ${file.totalLines}L",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!file.binary) {
                Spacer(modifier = Modifier.width(4.dp))
                if (editing) {
                    TextButton(onClick = {
                        onSave(file.path, editText)
                        editing = false
                    }, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("Save", fontSize = 12.sp) }
                    TextButton(onClick = {
                        editText = file.content
                        editing = false
                    }, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("\u2715", fontSize = 12.sp) }
                } else {
                    IconButton(onClick = { editing = true }, modifier = Modifier.size(28.dp)) {
                        Icon(Icons.Default.Create, "Edit", modifier = Modifier.size(16.dp))
                    }
                }
            }
        }

        if (file.binary && file.isImage) {
            ImageViewer(file.contentBase64)
        } else if (file.binary) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Binary file (${formatSize(file.contentBase64.length.toLong() * 3 / 4)})")
            }
        } else if (editing) {
            BasicTextField(
                value = editText,
                onValueChange = { editText = it },
                textStyle = TextStyle(
                    fontFamily = FontFamily.Monospace,
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurface,
                ),
                modifier = Modifier
                    .fillMaxSize()
                    .padding(8.dp)
                    .horizontalScroll(rememberScrollState()),
            )
        } else {
            CodeView(file.content, file.language)
        }
    }
}

// ── Code View with syntax highlighting ─────────────────────────────────

@Composable
private fun CodeView(content: String, language: String) {
    val lines = remember(content) { content.lines() }
    val lineNumWidth = remember(lines.size) { "${lines.size}".length }
    val codeColor = MaterialTheme.colorScheme.onSurface
    val lineNumColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .horizontalScroll(rememberScrollState()),
    ) {
        itemsIndexed(lines) { index, line ->
            Row(modifier = Modifier.padding(horizontal = 4.dp)) {
                Text(
                    text = "${index + 1}".padStart(lineNumWidth),
                    style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = lineNumColor),
                    modifier = Modifier.padding(end = 6.dp),
                )
                Text(
                    text = highlightLine(line, language, codeColor),
                    style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp),
                )
            }
        }
    }
}

private fun highlightLine(line: String, language: String, defaultColor: Color): AnnotatedString {
    if (language == "text" || language == "markdown") {
        return AnnotatedString(line)
    }
    return buildAnnotatedString {
        val commentPrefixes = listOf("//", "#", "--")
        val commentStart = commentPrefixes.map { line.indexOf(it) }.filter { it >= 0 }.minOrNull()
        val beforeComment = if (commentStart != null) line.substring(0, commentStart) else line
        val commentPart = if (commentStart != null) line.substring(commentStart) else ""

        val tokens = beforeComment.split(Regex("(?<=[\\s{}()\\[\\],;:=+\\-*/<>!&|^~@])|(?=[\\s{}()\\[\\],;:=+\\-*/<>!&|^~@])"))
        var inString = false
        var stringChar = ' '

        for (token in tokens) {
            when {
                inString -> {
                    withStyle(SpanStyle(color = strColor)) { append(token) }
                    if (token.endsWith(stringChar.toString()) && !token.endsWith("\\$stringChar")) {
                        inString = false
                    }
                }
                token.startsWith("\"") || token.startsWith("'") -> {
                    withStyle(SpanStyle(color = strColor)) { append(token) }
                    stringChar = token[0]
                    if (!(token.length > 1 && token.endsWith(stringChar.toString()))) {
                        inString = true
                    }
                }
                token in KEYWORDS -> {
                    withStyle(SpanStyle(color = kwColor, fontWeight = FontWeight.Bold)) { append(token) }
                }
                token.matches(Regex("-?\\d+\\.?\\d*[fFdDlL]?")) -> {
                    withStyle(SpanStyle(color = numColor)) { append(token) }
                }
                else -> {
                    withStyle(SpanStyle(color = defaultColor)) { append(token) }
                }
            }
        }

        if (commentPart.isNotEmpty()) {
            withStyle(SpanStyle(color = commentColor)) { append(commentPart) }
        }
    }
}

@Composable
private fun ImageViewer(base64Data: String) {
    val bitmap = remember(base64Data) {
        runCatching {
            val bytes = Base64.decode(base64Data, Base64.DEFAULT)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
        }.getOrNull()
    }
    Box(
        modifier = Modifier.fillMaxSize().padding(8.dp),
        contentAlignment = Alignment.Center,
    ) {
        if (bitmap != null) {
            Image(bitmap = bitmap, contentDescription = "Image", modifier = Modifier.fillMaxWidth())
        } else {
            Text("Unable to decode image")
        }
    }
}

// ── Terminal Panel ─────────────────────────────────────────────────────

@Composable
private fun TerminalPanel(
    results: List<CommandResult>,
    history: List<String>,
    onRun: (String, Int) -> Unit,
) {
    var command by remember { mutableStateOf("") }
    var showHistory by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(modifier = Modifier.weight(1f)) {
                OutlinedTextField(
                    value = command,
                    onValueChange = { command = it },
                    label = { Text("$ command") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    textStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp),
                    trailingIcon = {
                        if (history.isNotEmpty()) {
                            IconButton(onClick = { showHistory = !showHistory }, modifier = Modifier.size(24.dp)) {
                                Text("\u2191", fontSize = 14.sp)
                            }
                        }
                    }
                )
                DropdownMenu(expanded = showHistory, onDismissRequest = { showHistory = false }) {
                    history.take(15).forEach { cmd ->
                        DropdownMenuItem(
                            text = { Text(cmd, fontFamily = FontFamily.Monospace, fontSize = 12.sp, maxLines = 1) },
                            onClick = { command = cmd; showHistory = false },
                        )
                    }
                }
            }
            Spacer(modifier = Modifier.width(4.dp))
            IconButton(onClick = { if (command.isNotBlank()) onRun(command, 30) }) {
                Icon(Icons.Default.PlayArrow, "Run")
            }
        }

        LazyColumn(
            modifier = Modifier.fillMaxSize().background(Color(0xFF1A1A1A)),
        ) {
            items(results) { result ->
                TerminalResultBlock(result)
                Spacer(modifier = Modifier.height(2.dp))
            }
            if (results.isEmpty()) {
                item {
                    Box(modifier = Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
                        Text("Terminal ready", color = Color(0xFF666666), fontSize = 13.sp)
                    }
                }
            }
        }
    }
}

@Composable
private fun TerminalResultBlock(result: CommandResult) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(if (result.exitCode == 0) Color(0xFF1B3A1B) else Color(0xFF3A1B1B))
                .padding(horizontal = 10.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "$ ${result.command}",
                style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = Color(0xFFB0B0B0)),
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
            Text(
                "exit:${result.exitCode}",
                fontSize = 11.sp,
                fontWeight = FontWeight.Bold,
                color = if (result.exitCode == 0) Color(0xFF4CAF50) else Color(0xFFF44336),
            )
            if (result.elapsedS > 0) {
                Spacer(modifier = Modifier.width(6.dp))
                Text("${result.elapsedS}s", fontSize = 10.sp, color = Color(0xFF888888))
            }
        }
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 10.dp, vertical = 4.dp),
        ) {
            if (result.stdout.isNotBlank()) {
                Text(
                    result.stdout,
                    style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = Color(0xFFD4D4D4)),
                )
            }
            if (result.stderr.isNotBlank()) {
                Text(
                    result.stderr,
                    style = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = Color(0xFFEF5350)),
                )
            }
        }
    }
}

// ── Search Panel ───────────────────────────────────────────────────────

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SearchPanel(
    results: List<SearchMatch>,
    onSearch: (String, Boolean, String) -> Unit,
    onOpenMatch: (SearchMatch) -> Unit,
) {
    var query by remember { mutableStateOf("") }
    var isRegex by remember { mutableStateOf(false) }
    var includeFilter by remember { mutableStateOf("") }
    var showOptions by remember { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                label = { Text("Search in files") },
                modifier = Modifier.weight(1f),
                singleLine = true,
                textStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp),
            )
            Spacer(modifier = Modifier.width(4.dp))
            IconButton(onClick = { showOptions = !showOptions }, modifier = Modifier.size(32.dp)) {
                Text("\u2699", fontSize = 16.sp)
            }
            IconButton(onClick = { if (query.isNotBlank()) onSearch(query, isRegex, includeFilter) }) {
                Icon(Icons.Default.Search, "Search")
            }
        }

        if (showOptions) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Checkbox(checked = isRegex, onCheckedChange = { isRegex = it })
                Text("Regex", fontSize = 12.sp)
                Spacer(modifier = Modifier.width(12.dp))
                OutlinedTextField(
                    value = includeFilter,
                    onValueChange = { includeFilter = it },
                    label = { Text("Filter (.py,.kt)") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    textStyle = TextStyle(fontSize = 12.sp),
                )
            }
        }

        if (results.isNotEmpty()) {
            Text(
                "${results.size} matches",
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Divider()

        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(results) { match ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onOpenMatch(match) }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                ) {
                    Column {
                        Text(
                            "${match.file}:${match.line}",
                            style = MaterialTheme.typography.labelSmall.copy(
                                fontFamily = FontFamily.Monospace,
                                color = MaterialTheme.colorScheme.primary,
                            ),
                        )
                        Text(
                            match.text,
                            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                            maxLines = 2,
                        )
                    }
                }
                Divider()
            }
        }
    }
}

// ── Helpers ────────────────────────────────────────────────────────────

private fun formatSize(bytes: Long): String = when {
    bytes < 1024 -> "$bytes B"
    bytes < 1024 * 1024 -> "${bytes / 1024} KB"
    else -> "${"%.1f".format(bytes / 1024.0 / 1024.0)} MB"
}

private fun fileIcon(name: String): String {
    val ext = name.substringAfterLast(".", "").lowercase()
    return when (ext) {
        "py" -> "\uD83D\uDC0D"
        "kt", "kts" -> "\uD83C\uDF1F"
        "java" -> "\u2615"
        "js", "ts" -> "\uD83D\uDFE1"
        "rs" -> "\u2699"
        "json", "yaml", "yml", "toml" -> "\uD83D\uDCCB"
        "md" -> "\uD83D\uDCDD"
        "xml", "html" -> "\uD83C\uDF10"
        "png", "jpg", "jpeg", "gif", "svg" -> "\uD83D\uDDBC"
        "gradle" -> "\uD83D\uDC18"
        else -> "\uD83D\uDCC4"
    }
}
