<script setup>
import { ref, defineEmits, defineProps } from 'vue'

const props = defineProps({
  apiBase: String,
  userId: String
})
const emit = defineEmits(['task-created'])

const prompt = ref('Build me a production-grade agent runtime skeleton')

async function createTask() {
  const resp = await fetch(`${props.apiBase}/agents/tasks`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ tenant_id: 'default', user_id: props.userId, prompt: prompt.value })
  })
  const data = await resp.json()
  emit('task-created', data)
}
</script>

<template>
  <div class="section">
    <h3>Create Task</h3>
    <textarea v-model="prompt" rows="3" style="width:100%"></textarea>
    <div class="row" style="margin-top:8px">
      <button @click="createTask">Create Task</button>
    </div>
  </div>
</template>
