<script setup>
import { defineProps } from 'vue'

const props = defineProps({
  events: Array // AgentEvent[]
})
</script>

<template>
  <div class="section">
    <h3>Event Log</h3>
    <div class="log">
      <div class="item" v-for="(event, idx) in events" :key="idx">
        <div class="event-header">
          <strong :class="'type-' + event.event_type">{{ event.event_type }}</strong>
          <span class="muted ts" v-if="event.timestamp">{{ new Date(event.timestamp).toLocaleTimeString() }}</span>
        </div>
        <pre style="white-space:pre-wrap">{{ JSON.stringify(event.payload, null, 2) }}</pre>
      </div>
    </div>
  </div>
</template>

<style scoped>
.log {
  max-height: 380px;
  overflow-y: auto;
  border: 1px solid #d1d5db;
  border-radius: 8px;
  padding: 8px;
}
.item { border-bottom: 1px dashed #e5e7eb; padding: 6px 0; }
.item:last-child { border-bottom: none; }
.event-header { display: flex; gap: 8px; align-items: center; }
.ts { font-size: 0.8em; }
.type-state_event { color: #0f766e; }
.type-task_event { color: #1d4ed8; }
.type-tool_event { color: #d97706; }
.type-notification_event { color: #7c3aed; }
</style>
