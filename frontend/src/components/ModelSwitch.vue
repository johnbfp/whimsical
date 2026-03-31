<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({ apiBase: String })

const provider = ref('ollama')
const modelName = ref('llama3.1')
const result = ref('')

async function switchModel() {
  result.value = ''
  const resp = await fetch(`${props.apiBase}/models/switch`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ provider: provider.value, model_name: modelName.value })
  })
  const data = await resp.json()
  if (resp.ok) {
    result.value = `Switched to ${data.provider}/${data.model_name}`
  } else {
    result.value = `Error: ${data.detail?.detail || JSON.stringify(data.detail)}`
  }
}
</script>

<template>
  <div class="section">
    <h3>Model Switch</h3>
    <div class="row">
      <select v-model="provider">
        <option value="local">local (mock)</option>
        <option value="ollama">ollama</option>
        <option value="cloud">cloud (OpenAI)</option>
      </select>
      <input v-model="modelName" placeholder="model name" />
      <button @click="switchModel">Switch</button>
    </div>
    <p v-if="result" class="muted" style="margin-top:4px">{{ result }}</p>
  </div>
</template>
