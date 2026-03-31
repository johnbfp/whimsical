<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({ apiBase: String })

const userId = ref('demo-user')
const query = ref('')
const topK = ref(5)
const items = ref([])

async function recall() {
  const resp = await fetch(`${props.apiBase}/memory/recall`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ tenant_id: 'default', user_id: userId.value, query: query.value, top_k: topK.value })
  })
  if (resp.ok) {
    const data = await resp.json()
    items.value = data.items
  }
}
</script>

<template>
  <div class="section">
    <h3>Memory Recall</h3>
    <div class="row">
      <input v-model="userId" placeholder="user_id" style="width:120px" />
      <input v-model="query" placeholder="query" style="flex:1" />
      <input v-model="topK" type="number" placeholder="top_k" style="width:60px" />
      <button @click="recall">Recall</button>
    </div>
    <ul v-if="items.length" class="recall-list">
      <li v-for="(item, i) in items" :key="i">{{ item }}</li>
    </ul>
    <p class="muted" v-else style="margin-top:4px">No memories recalled.</p>
  </div>
</template>

<style scoped>
.recall-list { margin-top: 8px; padding-left: 20px; }
.recall-list li { margin: 4px 0; font-size: 0.9em; }
</style>
