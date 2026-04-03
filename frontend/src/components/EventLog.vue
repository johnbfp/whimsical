<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({
  events: Array
})

const expandedIdx = ref(null)

function typeClass(eventType) {
  if (eventType === 'state_event') return 'state'
  if (eventType === 'task_event') return 'task'
  if (eventType === 'tool_event') return 'tool'
  if (eventType === 'notification_event') return 'notification'
  return ''
}

function toggle(idx) {
  expandedIdx.value = expandedIdx.value === idx ? null : idx
}
</script>

<template>
  <div>
    <div v-if="!events || !events.length" style="color:var(--text-muted);font-size:13px;padding:8px 0;">No events yet.</div>
    <div class="event-list" v-else>
      <div
        class="event-item"
        v-for="(event, idx) in events"
        :key="idx"
        @click="toggle(idx)"
      >
        <div style="display:flex;align-items:center;justify-content:space-between;">
          <span class="event-type" :class="typeClass(event.event_type)">{{ event.event_type }}</span>
          <span class="event-time" v-if="event.timestamp">{{ new Date(event.timestamp).toLocaleTimeString() }}</span>
        </div>
        <div class="event-detail" v-if="expandedIdx === idx">
          {{ JSON.stringify(event.payload, null, 2) }}
        </div>
      </div>
    </div>
  </div>
</template>
