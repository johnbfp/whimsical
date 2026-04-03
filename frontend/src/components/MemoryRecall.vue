<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({ apiBase: String })

const userId = ref('demo-user')
const query = ref('')
const topK = ref(5)
const items = ref([])
const loading = ref(false)

async function recall() {
  loading.value = true
  try {
    const resp = await fetch(`${props.apiBase}/memory/recall`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ tenant_id: 'default', user_id: userId.value, query: query.value, top_k: topK.value })
    })
    if (resp.ok) {
      const data = await resp.json()
      items.value = data.items
    }
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div>
    <div class="form-group">
      <label class="form-label">User ID</label>
      <input class="form-input" v-model="userId" />
    </div>
    <div class="form-group">
      <label class="form-label">Search Query</label>
      <input class="form-input" v-model="query" placeholder="What are you looking for?" @keydown.enter="recall" />
    </div>
    <div class="form-group">
      <label class="form-label">Top K Results</label>
      <input class="form-input" v-model="topK" type="number" style="width:80px;" />
    </div>
    <button class="btn btn-primary" @click="recall" :disabled="loading" style="width:100%;">
      {{ loading ? 'Searching...' : 'Search Memory' }}
    </button>

    <ul v-if="items.length" class="recall-list" style="margin-top:14px;">
      <li v-for="(item, i) in items" :key="i">{{ item }}</li>
    </ul>
    <div v-else-if="!loading" style="color:var(--text-muted);font-size:13px;padding:12px 0;">No memories recalled.</div>
  </div>
</template>
