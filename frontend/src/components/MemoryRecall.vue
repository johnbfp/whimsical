<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({ apiBase: String })

const activeTab = ref('recall') // recall | write | search | history
const userId = ref('demo-user')
const loading = ref(false)
const error = ref('')
const toast = ref('')

// Recall
const recallQuery = ref('')
const recallTopK = ref(5)
const recallItems = ref([])

// Write
const writeKey = ref('')
const writeValue = ref('')

// Search
const searchQuery = ref('')
const searchTopK = ref(5)
const searchResults = ref([])

// History
const historyItems = ref([])

async function api(path, opts = {}) {
  const resp = await fetch(`${props.apiBase}${path}`, opts)
  if (!resp.ok) throw new Error(await resp.text() || resp.statusText)
  return resp.json()
}

async function recall() {
  loading.value = true; error.value = ''
  try {
    const data = await api('/memory/recall', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ tenant_id: 'default', user_id: userId.value, query: recallQuery.value, top_k: recallTopK.value })
    })
    recallItems.value = data.items || []
  } catch (e) { error.value = e.message } finally { loading.value = false }
}

async function writeMemory() {
  if (!writeKey.value.trim() || !writeValue.value.trim()) return
  loading.value = true; error.value = ''
  try {
    await api('/memory/write', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ tenant_id: 'default', user_id: userId.value, key: writeKey.value, value: writeValue.value })
    })
    toast.value = '✓ Memory saved'
    writeKey.value = ''; writeValue.value = ''
    setTimeout(() => { toast.value = '' }, 2000)
  } catch (e) { error.value = e.message } finally { loading.value = false }
}

async function searchMemory() {
  loading.value = true; error.value = ''
  try {
    const params = new URLSearchParams({ tenant_id: 'default', query: searchQuery.value, top_k: searchTopK.value })
    const data = await api(`/memory/search?${params}`)
    searchResults.value = data.results || []
  } catch (e) { error.value = e.message } finally { loading.value = false }
}

async function loadHistory() {
  loading.value = true; error.value = ''
  try {
    const params = new URLSearchParams({ user_id: userId.value, last_n: '20' })
    const data = await api(`/memory/history/${userId.value}?last_n=20`)
    historyItems.value = data.entries || []
  } catch (e) { error.value = e.message } finally { loading.value = false }
}
</script>

<template>
  <div>
    <!-- Tab selector -->
    <div style="display:flex;gap:4px;margin-bottom:12px;">
      <button v-for="t in ['recall','write','search','history']" :key="t"
        class="btn" :class="activeTab === t ? 'btn-primary' : 'btn-ghost'"
        @click="activeTab = t; if(t==='history') loadHistory()"
        style="flex:1;font-size:11px;padding:4px 0;">
        {{ t.charAt(0).toUpperCase() + t.slice(1) }}
      </button>
    </div>

    <div class="form-group">
      <label class="form-label">User ID</label>
      <input class="form-input" v-model="userId" />
    </div>

    <div class="inline-error" v-if="error" @click="error=''">{{ error }}</div>
    <div style="color:#4caf50;font-size:12px;margin:4px 0;" v-if="toast">{{ toast }}</div>

    <!-- RECALL TAB -->
    <template v-if="activeTab === 'recall'">
      <div class="form-group">
        <label class="form-label">Search Query</label>
        <input class="form-input" v-model="recallQuery" placeholder="What are you looking for?" @keydown.enter="recall" />
      </div>
      <div class="form-group">
        <label class="form-label">Top K</label>
        <input class="form-input" v-model="recallTopK" type="number" style="width:80px;" />
      </div>
      <button class="btn btn-primary" @click="recall" :disabled="loading" style="width:100%;">
        {{ loading ? 'Searching...' : 'Search Memory' }}
      </button>
      <ul v-if="recallItems.length" class="recall-list" style="margin-top:14px;">
        <li v-for="(item, i) in recallItems" :key="i">{{ item }}</li>
      </ul>
    </template>

    <!-- WRITE TAB -->
    <template v-if="activeTab === 'write'">
      <div class="form-group">
        <label class="form-label">Key</label>
        <input class="form-input" v-model="writeKey" placeholder="memory key" />
      </div>
      <div class="form-group">
        <label class="form-label">Value</label>
        <textarea class="form-input" v-model="writeValue" rows="4" placeholder="memory value" style="resize:vertical;"></textarea>
      </div>
      <button class="btn btn-primary" @click="writeMemory" :disabled="loading" style="width:100%;">
        {{ loading ? 'Saving...' : 'Save Memory' }}
      </button>
    </template>

    <!-- SEARCH TAB -->
    <template v-if="activeTab === 'search'">
      <div class="form-group">
        <label class="form-label">Semantic Search</label>
        <input class="form-input" v-model="searchQuery" placeholder="search memories..." @keydown.enter="searchMemory" />
      </div>
      <div class="form-group">
        <label class="form-label">Top K</label>
        <input class="form-input" v-model="searchTopK" type="number" style="width:80px;" />
      </div>
      <button class="btn btn-primary" @click="searchMemory" :disabled="loading" style="width:100%;">
        {{ loading ? 'Searching...' : 'Search' }}
      </button>
      <div v-if="searchResults.length" style="margin-top:14px;">
        <div v-for="(r, i) in searchResults" :key="i" style="padding:6px 0;border-bottom:1px solid var(--border,#333);">
          <div style="font-weight:600;font-size:12px;">{{ r.key }}</div>
          <div style="font-size:12px;color:var(--text-muted,#888);">{{ r.value }}</div>
        </div>
      </div>
    </template>

    <!-- HISTORY TAB -->
    <template v-if="activeTab === 'history'">
      <button class="btn btn-primary" @click="loadHistory" :disabled="loading" style="width:100%;margin-bottom:10px;">
        {{ loading ? 'Loading...' : 'Refresh History' }}
      </button>
      <div v-if="historyItems.length" style="font-size:12px;">
        <div v-for="(h, i) in historyItems" :key="i" style="padding:6px 0;border-bottom:1px solid var(--border,#333);">
          <div style="display:flex;justify-content:space-between;">
            <span style="font-weight:600;">{{ h.key || h.action || 'entry' }}</span>
            <span style="color:var(--text-muted,#666);font-size:11px;">{{ h.timestamp || '' }}</span>
          </div>
          <div style="color:var(--text-muted,#888);">{{ h.value || JSON.stringify(h) }}</div>
        </div>
      </div>
      <div v-else-if="!loading" style="text-align:center;color:var(--text-muted,#666);padding:20px;font-size:13px;">
        No history entries
      </div>
    </template>
    <div v-else-if="!loading" style="color:var(--text-muted);font-size:13px;padding:12px 0;">No memories recalled.</div>
  </div>
</template>
