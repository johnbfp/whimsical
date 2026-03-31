<script setup>
import { defineProps } from 'vue'

const props = defineProps({
  events: Array // AgentEvent[]
})

function toolEvents() {
  return (props.events || []).filter(e => e.event_type === 'tool_event')
}
</script>

<template>
  <div class="section" v-if="toolEvents().length">
    <h3>Tool Execution Trace</h3>
    <div class="trace-item" v-for="(evt, idx) in toolEvents()" :key="idx">
      <div class="trace-header">
        <span class="badge">{{ evt.payload.tool_name || 'tool' }}</span>
        <span class="status" :class="(evt.payload.status || '').toLowerCase()">{{ evt.payload.status }}</span>
        <span class="muted" v-if="evt.payload.execution_id">exec: {{ evt.payload.execution_id.slice(0, 8) }}…</span>
      </div>
      <pre v-if="evt.payload.result" class="result">{{ evt.payload.result }}</pre>
      <pre v-if="evt.payload.error" class="error">{{ evt.payload.error }}</pre>
    </div>
  </div>
</template>

<style scoped>
.trace-item {
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  padding: 8px 12px;
  margin-top: 6px;
}
.trace-header { display: flex; gap: 8px; align-items: center; }
.badge {
  display: inline-block;
  background: var(--accent);
  color: #fff;
  padding: 2px 10px;
  border-radius: 12px;
  font-size: 0.85em;
}
.status { font-weight: 600; font-size: 0.85em; }
.status.completed { color: #16a34a; }
.status.failed { color: #dc2626; }
.status.running { color: #d97706; }
.result, .error { margin: 4px 0 0; font-size: 0.85em; white-space: pre-wrap; }
.error { color: #dc2626; }
</style>
