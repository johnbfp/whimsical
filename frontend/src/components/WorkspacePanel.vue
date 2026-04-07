<script setup>
import { ref, computed, watch, nextTick } from 'vue'

const props = defineProps({ apiBase: String })

// ── State ───────────────────────────────────────────────────────────
const activeTab = ref('files')   // files | editor | terminal | search
const currentPath = ref('.')
const fileTree = ref([])
const loading = ref(false)
const error = ref('')
const toast = ref('')

// Editor
const openTabs = ref([])         // [{path, content, language, binary, totalLines}]
const activeTabIdx = ref(-1)
const editing = ref(false)
const editText = ref('')

// Terminal
const cmdInput = ref('')
const cmdResults = ref([])
const cmdHistory = ref([])
const showHistory = ref(false)

// Search
const searchQuery = ref('')
const searchRegex = ref(false)
const searchInclude = ref('')
const searchResults = ref([])

// Dialogs
const dialog = ref(null)  // 'newFile' | 'newFolder' | 'rename' | 'delete'
const dialogName = ref('')
const dialogTarget = ref(null)

// ── Computed ────────────────────────────────────────────────────────
const currentFile = computed(() =>
  activeTabIdx.value >= 0 && activeTabIdx.value < openTabs.value.length
    ? openTabs.value[activeTabIdx.value]
    : null
)

const breadcrumb = computed(() => {
  if (currentPath.value === '.') return ['/']
  return ['/', ...currentPath.value.split('/')]
})

// ── Toast auto-clear ────────────────────────────────────────────────
let toastTimer = null
function showToast(msg) {
  toast.value = msg
  clearTimeout(toastTimer)
  toastTimer = setTimeout(() => { toast.value = '' }, 2500)
}

// ── API helpers ─────────────────────────────────────────────────────
async function api(path, opts = {}) {
  const resp = await fetch(`${props.apiBase}${path}`, opts)
  if (!resp.ok) {
    const t = await resp.text()
    throw new Error(t || resp.statusText)
  }
  return resp.json()
}

// ── File Tree ───────────────────────────────────────────────────────
async function loadTree(path) {
  if (path !== undefined) currentPath.value = path
  loading.value = true
  error.value = ''
  try {
    const p = encodeURIComponent(currentPath.value)
    const data = await api(`/workspace/tree?path=${p}&depth=1`)
    fileTree.value = data.entries || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function navigateUp() {
  if (currentPath.value === '.') return
  const parent = currentPath.value.includes('/')
    ? currentPath.value.substring(0, currentPath.value.lastIndexOf('/'))
    : '.'
  loadTree(parent)
}

function onEntryClick(entry) {
  if (entry.is_dir) {
    loadTree(entry.path)
  } else {
    openFile(entry.path)
  }
}

// ── File Operations ─────────────────────────────────────────────────
async function openFile(path) {
  const existing = openTabs.value.findIndex(t => t.path === path)
  if (existing >= 0) {
    activeTabIdx.value = existing
    activeTab.value = 'editor'
    return
  }
  loading.value = true
  try {
    const p = encodeURIComponent(path)
    const data = await api(`/workspace/file?path=${p}`)
    openTabs.value.push(data)
    activeTabIdx.value = openTabs.value.length - 1
    activeTab.value = 'editor'
    editing.value = false
    editText.value = data.content || ''
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function closeTabAt(idx) {
  openTabs.value.splice(idx, 1)
  if (openTabs.value.length === 0) {
    activeTabIdx.value = -1
  } else if (activeTabIdx.value >= openTabs.value.length) {
    activeTabIdx.value = openTabs.value.length - 1
  }
}

async function saveFile() {
  const f = currentFile.value
  if (!f) return
  loading.value = true
  try {
    await api('/workspace/file', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: f.path, content: editText.value })
    })
    f.content = editText.value
    f.total_lines = editText.value.split('\n').length
    editing.value = false
    showToast('✓ Saved ' + f.path)
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function createNewItem() {
  const isFolder = dialog.value === 'newFolder'
  const name = dialogName.value.trim()
  if (!name) return
  const fullPath = currentPath.value === '.' ? name : `${currentPath.value}/${name}`
  loading.value = true
  try {
    if (isFolder) {
      await api('/workspace/mkdir', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: fullPath })
      })
    } else {
      await api('/workspace/file', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: fullPath, content: '' })
      })
    }
    showToast(`✓ Created ${fullPath}`)
    dialog.value = null
    loadTree()
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function deleteItem() {
  if (!dialogTarget.value) return
  loading.value = true
  try {
    const p = encodeURIComponent(dialogTarget.value.path)
    await api(`/workspace/file?path=${p}`, { method: 'DELETE' })
    showToast(`✓ Deleted ${dialogTarget.value.name}`)
    // close tab if open
    const idx = openTabs.value.findIndex(t => t.path === dialogTarget.value.path)
    if (idx >= 0) closeTabAt(idx)
    dialog.value = null
    dialogTarget.value = null
    loadTree()
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function renameItem() {
  if (!dialogTarget.value || !dialogName.value.trim()) return
  const oldPath = dialogTarget.value.path
  const parentDir = oldPath.includes('/') ? oldPath.substring(0, oldPath.lastIndexOf('/')) : ''
  const newPath = parentDir ? `${parentDir}/${dialogName.value.trim()}` : dialogName.value.trim()
  loading.value = true
  try {
    await api('/workspace/rename', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ old_path: oldPath, new_path: newPath })
    })
    showToast(`✓ Renamed → ${newPath}`)
    dialog.value = null
    dialogTarget.value = null
    loadTree()
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

// ── Terminal ────────────────────────────────────────────────────────
async function runCommand() {
  const cmd = cmdInput.value.trim()
  if (!cmd) return
  // add to history (dedup)
  cmdHistory.value = [cmd, ...cmdHistory.value.filter(c => c !== cmd)].slice(0, 50)
  showHistory.value = false
  loading.value = true
  try {
    const data = await api('/workspace/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ command: cmd, timeout: 30 })
    })
    cmdResults.value.unshift({ ...data, command: cmd })
    if (cmdResults.value.length > 50) cmdResults.value.length = 50
  } catch (e) {
    cmdResults.value.unshift({ exit_code: -1, stdout: '', stderr: e.message, command: cmd })
  } finally {
    loading.value = false
  }
}

// ── Search ──────────────────────────────────────────────────────────
async function doSearch() {
  const q = searchQuery.value.trim()
  if (!q) return
  loading.value = true
  try {
    const params = new URLSearchParams({ query: q, path: '.' })
    if (searchRegex.value) params.set('is_regex', 'true')
    if (searchInclude.value) params.set('include', searchInclude.value)
    const data = await api(`/workspace/search?${params}`)
    searchResults.value = data.matches || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function onSearchMatchClick(m) {
  openFile(m.file)
}

// ── File icon helper ────────────────────────────────────────────────
function fileIcon(name) {
  const ext = name.split('.').pop().toLowerCase()
  const icons = {
    py: '🐍', kt: '🌟', kts: '🌟', java: '☕', js: '🟡', ts: '🟡',
    rs: '⚙️', json: '📋', yaml: '📋', yml: '📋', toml: '📋',
    md: '📝', xml: '🌐', html: '🌐', vue: '💚', css: '🎨',
    png: '🖼️', jpg: '🖼️', jpeg: '🖼️', gif: '🖼️', svg: '🖼️',
    gradle: '🐘', sh: '💻', bat: '💻', ps1: '💻', dockerfile: '🐳',
  }
  return icons[ext] || '📄'
}

function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${Math.floor(bytes / 1024)} KB`
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

// Init
loadTree('.')
</script>

<template>
  <div class="ws-panel">
    <!-- Tab bar -->
    <div class="ws-tabs">
      <button
        v-for="t in ['files','editor','terminal','search']"
        :key="t"
        class="ws-tab"
        :class="{ active: activeTab === t }"
        @click="activeTab = t"
      >
        {{ t === 'editor' ? `Editor (${openTabs.length})` : t.charAt(0).toUpperCase() + t.slice(1) }}
      </button>
    </div>

    <!-- Status -->
    <div class="ws-status" v-if="loading">
      <div class="ws-progress"></div>
    </div>
    <div class="ws-error" v-if="error" @click="error = ''">{{ error }}</div>
    <div class="ws-toast" v-if="toast">{{ toast }}</div>

    <!-- FILES TAB ──────────────────────────────────────── -->
    <div class="ws-content" v-if="activeTab === 'files'">
      <div class="ws-toolbar">
        <button class="ws-icon-btn" @click="navigateUp" :disabled="currentPath === '.'">⬆</button>
        <span class="ws-breadcrumb">{{ currentPath === '.' ? '/' : '/' + currentPath }}</span>
        <div style="flex:1"></div>
        <button class="ws-icon-btn" @click="dialog = 'newFile'; dialogName = ''" title="New File">📄+</button>
        <button class="ws-icon-btn" @click="dialog = 'newFolder'; dialogName = ''" title="New Folder">📁+</button>
        <button class="ws-icon-btn" @click="loadTree()" title="Refresh">🔄</button>
      </div>
      <div class="ws-file-list">
        <div
          v-for="entry in fileTree"
          :key="entry.path"
          class="ws-file-row"
          @click="onEntryClick(entry)"
        >
          <span class="ws-file-icon">{{ entry.is_dir ? '📁' : fileIcon(entry.name) }}</span>
          <span class="ws-file-name" :class="{ dir: entry.is_dir }">{{ entry.name }}</span>
          <span class="ws-file-size" v-if="!entry.is_dir && entry.size > 0">{{ formatSize(entry.size) }}</span>
          <div class="ws-file-actions">
            <button class="ws-tiny-btn" @click.stop="dialogTarget = entry; dialogName = entry.name; dialog = 'rename'">✏️</button>
            <button class="ws-tiny-btn" @click.stop="dialogTarget = entry; dialog = 'delete'">🗑️</button>
          </div>
        </div>
        <div class="ws-empty" v-if="!fileTree.length && !loading">Empty directory</div>
      </div>
    </div>

    <!-- EDITOR TAB ─────────────────────────────────────── -->
    <div class="ws-content" v-if="activeTab === 'editor'">
      <div class="ws-empty" v-if="!openTabs.length">
        No files open — click a file in Files tab
      </div>
      <template v-else>
        <!-- Tab strip -->
        <div class="ws-editor-tabs">
          <div
            v-for="(tab, idx) in openTabs"
            :key="tab.path"
            class="ws-editor-tab"
            :class="{ active: idx === activeTabIdx }"
            @click="activeTabIdx = idx; editing = false; editText = tab.content || ''"
          >
            <span>{{ tab.path.split('/').pop() }}</span>
            <button class="ws-tab-close" @click.stop="closeTabAt(idx)">×</button>
          </div>
        </div>
        <!-- File content -->
        <div class="ws-editor-header" v-if="currentFile">
          <span class="ws-filepath">{{ currentFile.path }}</span>
          <span class="ws-filemeta">{{ currentFile.language || 'text' }} · {{ currentFile.total_lines || 0 }}L</span>
          <div style="flex:1"></div>
          <template v-if="!currentFile.binary">
            <button class="ws-icon-btn" v-if="!editing" @click="editing = true; editText = currentFile.content || ''">✏️ Edit</button>
            <template v-else>
              <button class="ws-icon-btn ws-save-btn" @click="saveFile">💾 Save</button>
              <button class="ws-icon-btn" @click="editing = false">Cancel</button>
            </template>
          </template>
        </div>
        <div class="ws-editor-body" v-if="currentFile">
          <textarea
            v-if="editing"
            class="ws-code-textarea"
            v-model="editText"
            spellcheck="false"
          ></textarea>
          <div v-else-if="currentFile.binary" class="ws-empty">
            Binary file ({{ formatSize((currentFile.content_base64 || '').length * 3 / 4) }})
          </div>
          <pre v-else class="ws-code-view"><code>{{ currentFile.content }}</code></pre>
        </div>
      </template>
    </div>

    <!-- TERMINAL TAB ───────────────────────────────────── -->
    <div class="ws-content" v-if="activeTab === 'terminal'">
      <div class="ws-terminal-input">
        <span class="ws-prompt">$</span>
        <input
          class="ws-cmd-input"
          v-model="cmdInput"
          placeholder="Enter command..."
          @keydown.enter="runCommand"
          @keydown.up="showHistory = !showHistory"
        />
        <button class="ws-icon-btn" @click="runCommand" :disabled="loading">▶</button>
      </div>
      <!-- History dropdown -->
      <div class="ws-cmd-history" v-if="showHistory && cmdHistory.length">
        <div
          v-for="(h, i) in cmdHistory.slice(0, 15)"
          :key="i"
          class="ws-history-item"
          @click="cmdInput = h; showHistory = false"
        >{{ h }}</div>
      </div>
      <!-- Results -->
      <div class="ws-terminal-output">
        <div v-for="(r, i) in cmdResults" :key="i" class="ws-term-block" :class="{ ok: r.exit_code === 0, fail: r.exit_code !== 0 }">
          <div class="ws-term-header">
            <span class="ws-term-cmd">$ {{ r.command }}</span>
            <span class="ws-term-exit" :class="{ ok: r.exit_code === 0, fail: r.exit_code !== 0 }">
              exit:{{ r.exit_code }}
            </span>
            <span class="ws-term-time" v-if="r.elapsed_s">{{ r.elapsed_s.toFixed(2) }}s</span>
          </div>
          <pre class="ws-term-stdout" v-if="r.stdout">{{ r.stdout }}</pre>
          <pre class="ws-term-stderr" v-if="r.stderr">{{ r.stderr }}</pre>
        </div>
        <div class="ws-empty" v-if="!cmdResults.length">Terminal ready</div>
      </div>
    </div>

    <!-- SEARCH TAB ─────────────────────────────────────── -->
    <div class="ws-content" v-if="activeTab === 'search'">
      <div class="ws-search-bar">
        <input
          class="ws-search-input"
          v-model="searchQuery"
          placeholder="Search in files..."
          @keydown.enter="doSearch"
        />
        <button class="ws-icon-btn" @click="doSearch" :disabled="loading">🔍</button>
      </div>
      <div class="ws-search-opts">
        <label><input type="checkbox" v-model="searchRegex" /> Regex</label>
        <input class="ws-search-filter" v-model="searchInclude" placeholder="Filter (.py,.kt)" />
      </div>
      <div class="ws-search-count" v-if="searchResults.length">
        {{ searchResults.length }} matches
      </div>
      <div class="ws-search-results">
        <div
          v-for="(m, i) in searchResults"
          :key="i"
          class="ws-search-match"
          @click="onSearchMatchClick(m)"
        >
          <div class="ws-match-file">{{ m.file }}:{{ m.line }}</div>
          <div class="ws-match-text">{{ m.text }}</div>
        </div>
      </div>
    </div>

    <!-- DIALOGS ────────────────────────────────────────── -->
    <div class="ws-dialog-overlay" v-if="dialog" @click="dialog = null">
      <div class="ws-dialog" @click.stop>
        <h4 v-if="dialog === 'newFile'">New File</h4>
        <h4 v-else-if="dialog === 'newFolder'">New Folder</h4>
        <h4 v-else-if="dialog === 'rename'">Rename</h4>
        <h4 v-else-if="dialog === 'delete'">Confirm Delete</h4>

        <div v-if="dialog === 'delete'" class="ws-dialog-body">
          Delete <strong>{{ dialogTarget?.name }}</strong>?
        </div>
        <div v-else class="ws-dialog-body">
          <input class="ws-dialog-input" v-model="dialogName" placeholder="Name" @keydown.enter="dialog === 'rename' ? renameItem() : createNewItem()" />
        </div>

        <div class="ws-dialog-actions">
          <button class="btn btn-ghost" @click="dialog = null">Cancel</button>
          <button
            v-if="dialog === 'delete'"
            class="btn btn-danger"
            @click="deleteItem"
          >Delete</button>
          <button
            v-else-if="dialog === 'rename'"
            class="btn btn-primary"
            @click="renameItem"
          >Rename</button>
          <button
            v-else
            class="btn btn-primary"
            @click="createNewItem"
          >Create</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.ws-panel {
  display: flex;
  flex-direction: column;
  height: 100%;
  font-family: -apple-system, 'Segoe UI', sans-serif;
}

.ws-tabs {
  display: flex;
  border-bottom: 1px solid var(--border, #333);
  background: var(--bg-secondary, #1a1a2e);
}
.ws-tab {
  padding: 6px 14px;
  background: none;
  border: none;
  color: var(--text-muted, #888);
  cursor: pointer;
  font-size: 12px;
  border-bottom: 2px solid transparent;
}
.ws-tab.active {
  color: var(--accent, #6c63ff);
  border-bottom-color: var(--accent, #6c63ff);
}

.ws-status {
  height: 2px;
  background: var(--bg-secondary, #1a1a2e);
}
.ws-progress {
  height: 100%;
  width: 40%;
  background: var(--accent, #6c63ff);
  animation: ws-prog 1s ease-in-out infinite alternate;
}
@keyframes ws-prog { from { margin-left: 0 } to { margin-left: 60% } }

.ws-error {
  background: rgba(255, 80, 80, 0.15);
  color: #f55;
  padding: 4px 10px;
  font-size: 12px;
  cursor: pointer;
}
.ws-toast {
  color: #4caf50;
  font-weight: bold;
  padding: 3px 12px;
  font-size: 12px;
}

.ws-content {
  flex: 1;
  overflow: auto;
  display: flex;
  flex-direction: column;
}

/* ── Files ─────────────────────────── */
.ws-toolbar {
  display: flex;
  align-items: center;
  padding: 4px 8px;
  gap: 6px;
  border-bottom: 1px solid var(--border, #333);
}
.ws-breadcrumb {
  font-family: monospace;
  font-size: 12px;
  color: var(--text-muted, #888);
}
.ws-icon-btn {
  background: none;
  border: 1px solid var(--border, #444);
  border-radius: 4px;
  padding: 2px 8px;
  color: var(--text, #ccc);
  cursor: pointer;
  font-size: 12px;
  white-space: nowrap;
}
.ws-icon-btn:hover { background: var(--bg-hover, rgba(255,255,255,0.05)); }
.ws-save-btn { border-color: #4caf50; color: #4caf50; }

.ws-file-list {
  flex: 1;
  overflow: auto;
}
.ws-file-row {
  display: flex;
  align-items: center;
  padding: 6px 12px;
  cursor: pointer;
  gap: 8px;
  border-bottom: 1px solid rgba(255,255,255,0.04);
}
.ws-file-row:hover { background: var(--bg-hover, rgba(255,255,255,0.04)); }
.ws-file-icon { font-size: 14px; flex-shrink: 0; }
.ws-file-name { flex: 1; font-size: 13px; }
.ws-file-name.dir { font-weight: 600; }
.ws-file-size { font-size: 11px; color: var(--text-muted, #666); }
.ws-file-actions { display: flex; gap: 2px; }
.ws-tiny-btn {
  background: none; border: none; cursor: pointer; font-size: 12px; padding: 2px 4px;
  opacity: 0.4;
}
.ws-tiny-btn:hover { opacity: 1; }
.ws-empty {
  padding: 30px;
  text-align: center;
  color: var(--text-muted, #666);
  font-size: 13px;
}

/* ── Editor ────────────────────────── */
.ws-editor-tabs {
  display: flex;
  overflow-x: auto;
  background: var(--bg-secondary, #1a1a2e);
  border-bottom: 1px solid var(--border, #333);
}
.ws-editor-tab {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 5px 12px;
  font-size: 12px;
  cursor: pointer;
  border-right: 1px solid var(--border, #333);
  color: var(--text-muted, #888);
  white-space: nowrap;
}
.ws-editor-tab.active {
  background: var(--bg, #151524);
  color: var(--text, #eee);
}
.ws-tab-close {
  background: none; border: none; color: inherit; cursor: pointer; font-size: 14px;
  opacity: 0.5;
}
.ws-tab-close:hover { opacity: 1; }

.ws-editor-header {
  display: flex;
  align-items: center;
  padding: 3px 10px;
  gap: 10px;
  border-bottom: 1px solid var(--border, #333);
  background: var(--bg-secondary, #1a1a2e);
}
.ws-filepath { font-family: monospace; font-size: 11px; color: var(--text-muted, #888); }
.ws-filemeta { font-size: 11px; color: var(--text-muted, #666); }

.ws-editor-body { flex: 1; overflow: auto; }
.ws-code-view {
  margin: 0;
  padding: 8px 12px;
  font-family: 'Cascadia Code', 'Fira Code', 'Consolas', monospace;
  font-size: 13px;
  line-height: 1.5;
  color: var(--text, #d4d4d4);
  white-space: pre;
  overflow-x: auto;
  tab-size: 4;
}
.ws-code-textarea {
  width: 100%;
  height: 100%;
  min-height: 200px;
  padding: 8px 12px;
  font-family: 'Cascadia Code', 'Fira Code', 'Consolas', monospace;
  font-size: 13px;
  line-height: 1.5;
  background: var(--bg, #151524);
  color: var(--text, #d4d4d4);
  border: none;
  outline: none;
  resize: none;
  tab-size: 4;
}

/* ── Terminal ──────────────────────── */
.ws-terminal-input {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 8px;
  border-bottom: 1px solid var(--border, #333);
}
.ws-prompt { font-family: monospace; color: #4caf50; font-weight: bold; }
.ws-cmd-input {
  flex: 1;
  background: var(--bg-secondary, #1a1a2e);
  border: 1px solid var(--border, #444);
  border-radius: 4px;
  padding: 6px 10px;
  font-family: monospace;
  font-size: 13px;
  color: var(--text, #d4d4d4);
}
.ws-cmd-history {
  background: var(--bg-secondary, #1a1a2e);
  border: 1px solid var(--border, #444);
  max-height: 200px;
  overflow: auto;
}
.ws-history-item {
  padding: 4px 12px;
  font-family: monospace;
  font-size: 12px;
  cursor: pointer;
  color: var(--text-muted, #888);
}
.ws-history-item:hover { background: var(--bg-hover, rgba(255,255,255,0.06)); }

.ws-terminal-output {
  flex: 1;
  overflow: auto;
  background: #111;
}
.ws-term-block {
  border-bottom: 1px solid rgba(255,255,255,0.06);
}
.ws-term-header {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 10px;
}
.ws-term-header { background: rgba(27, 58, 27, 0.5); }
.ws-term-block.fail .ws-term-header { background: rgba(58, 27, 27, 0.5); }
.ws-term-cmd { font-family: monospace; font-size: 12px; color: #b0b0b0; flex: 1; }
.ws-term-exit { font-size: 11px; font-weight: bold; }
.ws-term-exit.ok { color: #4caf50; }
.ws-term-exit.fail { color: #f44336; }
.ws-term-time { font-size: 10px; color: #666; }
.ws-term-stdout { margin: 0; padding: 4px 10px; font-family: monospace; font-size: 12px; color: #d4d4d4; white-space: pre-wrap; word-break: break-all; }
.ws-term-stderr { margin: 0; padding: 4px 10px; font-family: monospace; font-size: 12px; color: #ef5350; white-space: pre-wrap; word-break: break-all; }

/* ── Search ────────────────────────── */
.ws-search-bar {
  display: flex;
  gap: 6px;
  padding: 8px;
}
.ws-search-input {
  flex: 1;
  background: var(--bg-secondary, #1a1a2e);
  border: 1px solid var(--border, #444);
  border-radius: 4px;
  padding: 6px 10px;
  font-family: monospace;
  font-size: 13px;
  color: var(--text, #d4d4d4);
}
.ws-search-opts {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 0 10px 6px;
  font-size: 12px;
  color: var(--text-muted, #888);
}
.ws-search-filter {
  background: var(--bg-secondary, #1a1a2e);
  border: 1px solid var(--border, #444);
  border-radius: 4px;
  padding: 3px 8px;
  font-size: 12px;
  color: var(--text, #d4d4d4);
  width: 120px;
}
.ws-search-count {
  padding: 2px 12px;
  font-size: 11px;
  color: var(--text-muted, #666);
}
.ws-search-results { flex: 1; overflow: auto; }
.ws-search-match {
  padding: 6px 12px;
  cursor: pointer;
  border-bottom: 1px solid rgba(255,255,255,0.04);
}
.ws-search-match:hover { background: var(--bg-hover, rgba(255,255,255,0.04)); }
.ws-match-file { font-family: monospace; font-size: 11px; color: var(--accent, #6c63ff); }
.ws-match-text { font-family: monospace; font-size: 12px; color: var(--text, #ccc); margin-top: 2px; }

/* ── Dialogs ───────────────────────── */
.ws-dialog-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.5);
  z-index: 1000;
  display: flex;
  align-items: center;
  justify-content: center;
}
.ws-dialog {
  background: var(--bg, #1e1e2e);
  border-radius: 8px;
  padding: 20px;
  min-width: 300px;
  box-shadow: 0 10px 40px rgba(0,0,0,0.5);
}
.ws-dialog h4 { margin: 0 0 12px; font-size: 14px; }
.ws-dialog-body { margin-bottom: 14px; font-size: 13px; }
.ws-dialog-input {
  width: 100%;
  background: var(--bg-secondary, #1a1a2e);
  border: 1px solid var(--border, #444);
  border-radius: 4px;
  padding: 8px 10px;
  font-size: 13px;
  color: var(--text, #d4d4d4);
}
.ws-dialog-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.btn-danger {
  background: #f44336 !important;
  color: white !important;
}
</style>
