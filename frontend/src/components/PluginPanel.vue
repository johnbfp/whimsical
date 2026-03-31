<script setup>
import { ref, defineProps, onMounted } from 'vue'

const props = defineProps({ apiBase: String })

const plugins = ref([])
const pluginId = ref('')
const version = ref('1.0.0')
const toolName = ref('')
const permissions = ref('tool.execute')
const timeoutMs = ref(4000)

async function loadPlugins() {
  const resp = await fetch(`${props.apiBase}/plugins`)
  if (resp.ok) plugins.value = await resp.json()
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
  <div class="section">
    <h3>Plugin Manager</h3>
    <div class="row">
      <input v-model="pluginId" placeholder="plugin_id" style="width:120px" />
      <input v-model="toolName" placeholder="tool_name" style="width:120px" />
      <input v-model="version" placeholder="version" style="width:80px" />
      <input v-model="timeoutMs" type="number" placeholder="timeout_ms" style="width:90px" />
      <button @click="installPlugin">Install</button>
    </div>
    <table class="plugin-table" v-if="plugins.length">
      <thead><tr><th>ID</th><th>Tool</th><th>Version</th><th>Enabled</th><th>Actions</th></tr></thead>
      <tbody>
        <tr v-for="p in plugins" :key="p.plugin_id">
          <td>{{ p.plugin_id }}</td>
          <td>{{ p.tool_name }}</td>
          <td>{{ p.version }}</td>
          <td>{{ p.enabled ? 'Yes' : 'No' }}</td>
          <td>
            <button class="small" @click="togglePlugin(p.plugin_id, p.enabled)">{{ p.enabled ? 'Disable' : 'Enable' }}</button>
            <button class="small danger" @click="removePlugin(p.plugin_id)">Uninstall</button>
          </td>
        </tr>
      </tbody>
    </table>
    <p class="muted" v-else>No plugins installed.</p>
  </div>
</template>

<style scoped>
.plugin-table { width: 100%; border-collapse: collapse; margin-top: 8px; font-size: 0.9em; }
.plugin-table th, .plugin-table td { text-align: left; padding: 6px 8px; border-bottom: 1px solid #e5e7eb; }
.plugin-table th { background: #f1f5f9; }
.small { font-size: 0.8em; padding: 4px 8px; }
.danger { background: #dc2626; }
</style>
