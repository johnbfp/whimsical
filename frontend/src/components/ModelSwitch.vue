<script setup>
import { ref, watch, defineProps } from 'vue'

const props = defineProps({ apiBase: String })

const PROVIDER_DEFAULT_MODELS = {
  ollama: 'llama3.1',
  cloud: 'qwen3-coder-plus',
  local: 'mock',
}

const provider = ref('ollama')
const modelName = ref('llama3.1')

watch(provider, (val) => {
  modelName.value = PROVIDER_DEFAULT_MODELS[val] ?? ''
})
const result = ref('')
const loading = ref(false)

async function switchModel() {
  result.value = ''
  loading.value = true
  try {
    const resp = await fetch(`${props.apiBase}/models/switch`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ provider: provider.value, model_name: modelName.value })
    })
    const data = await resp.json()
    if (resp.ok) {
      result.value = `✓ Switched to ${data.provider}/${data.model_name}`
    } else {
      result.value = `✗ ${data.detail?.detail || JSON.stringify(data.detail)}`
    }
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div>
    <div class="form-group">
      <label class="form-label">Provider</label>
      <select class="form-select" v-model="provider">
        <option value="local">Local (Mock)</option>
        <option value="ollama">Ollama</option>
        <option value="cloud">Cloud (OpenAI)</option>
      </select>
    </div>
    <div class="form-group">
      <label class="form-label">Model Name</label>
      <input class="form-input" v-model="modelName" placeholder="e.g. llama3.1" />
    </div>
    <button class="btn btn-primary" @click="switchModel" :disabled="loading" style="width:100%">
      {{ loading ? 'Switching...' : 'Switch Model' }}
    </button>
    <div v-if="result" style="margin-top:10px;font-size:13px;padding:8px 12px;border-radius:var(--radius-sm);" :style="{
      background: result.startsWith('✓') ? 'var(--success-bg)' : 'var(--error-bg)',
      color: result.startsWith('✓') ? 'var(--success)' : 'var(--error)',
    }">
      {{ result }}
    </div>
  </div>
</template>
