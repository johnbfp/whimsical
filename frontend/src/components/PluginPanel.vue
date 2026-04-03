<script setup>
import { ref, defineProps, onMounted } from 'vue'

const props = defineProps({ apiBase: String })

const plugins = ref([])
const pluginId = ref('')
const version = ref('1.0.0')
const toolName = ref('')
const permissions = ref('tool.execute')
const timeoutMs = ref(4000)
const showForm = ref(false)

async function loadPlugins() {
  try {
    const resp = await fetch(`${props.apiBase}/plugins`)
    if (resp.ok) plugins.value = await resp.json()
  } catch { /* ignore */ }
}

async function installPlugin() {
  const manifest = {
    plugin_id: pluginId.value,
    version: version.value,
    tool_name: toolName.value,
    permissions: permissions.value.split(',').map(s => s.trim()),
    input_schema: {},
    timeout_ms: timeoutMs.value
  }
  await fetch(`${props.apiBase}/plugins/install`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ manifest })
  })
  pluginId.value = ''
  toolName.value = ''
  showForm.value = false
  await loadPlugins()
}

async function togglePlugin(id, enabled) {
  const action = enabled ? 'disable' : 'enable'
  await fetch(`${props.apiBase}/plugins/${id}/${action}`, { method: 'POST' })
  await loadPlugins()
}

async function removePlugin(id) {
  await fetch(`${props.apiBase}/plugins/${id}`, { method: 'DELETE' })
  await loadPlugins()
}

onMounted(loadPlugins)
</script>

<template>
  <div>
    <button class="btn btn-ghost btn-sm" @click="showForm = !showForm" style="margin-bottom:12px;">
      {{ showForm ? '✕ Cancel' : '+ Install Plugin' }}
    </button>

    <div v-if="showForm" style="margin-bottom:16px;padding:14px;background:#1a1a1a;border-radius:var(--radius-sm);">
      <div class="form-row" style="margin-bottom:8px;">
        <div class="form-group" style="flex:1;margin-bottom:0;">
          <label class="form-label">Plugin ID</label>
          <input class="form-input" v-model="pluginId" placeholder="my-plugin" />
        </div>
        <div class="form-group" style="flex:1;margin-bottom:0;">
          <label class="form-label">Tool Name</label>
          <input class="form-input" v-model="toolName" placeholder="my_tool" />
        </div>
      </div>
      <div class="form-row" style="margin-bottom:10px;">
        <div class="form-group" style="flex:1;margin-bottom:0;">
          <label class="form-label">Version</label>
          <input class="form-input" v-model="version" />
        </div>
        <div class="form-group" style="flex:1;margin-bottom:0;">
          <label class="form-label">Timeout (ms)</label>
          <input class="form-input" v-model="timeoutMs" type="number" />
        </div>
      </div>
      <button class="btn btn-primary btn-sm" @click="installPlugin">Install</button>
    </div>

    <div v-if="plugins.length">
      <table class="plugin-table">
        <thead>
          <tr><th>Plugin</th><th>Tool</th><th>Ver</th><th>Status</th><th></th></tr>
        </thead>
        <tbody>
          <tr v-for="p in plugins" :key="p.plugin_id">
            <td style="font-weight:500;">{{ p.plugin_id }}</td>
            <td><span class="tag tag-accent">{{ p.tool_name }}</span></td>
            <td>{{ p.version }}</td>
            <td>
              <span class="tag" :class="p.enabled ? 'tag-success' : 'tag-error'">{{ p.enabled ? 'Active' : 'Off' }}</span>
            </td>
            <td style="text-align:right;">
              <button class="btn btn-ghost btn-sm" @click="togglePlugin(p.plugin_id, p.enabled)">{{ p.enabled ? 'Disable' : 'Enable' }}</button>
              <button class="btn btn-danger btn-sm" style="margin-left:4px;" @click="removePlugin(p.plugin_id)">Remove</button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
    <div v-else style="color:var(--text-muted);font-size:13px;padding:8px 0;">No plugins installed.</div>
  </div>
</template>
