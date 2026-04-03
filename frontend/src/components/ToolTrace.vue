<script setup>
import { ref, defineProps, computed } from 'vue'

const props = defineProps({
  events: Array
})

const expanded = ref(false)

const toolEvents = computed(() =>
  (props.events || []).filter(e => e.event_type === 'tool_event')
)
</script>

<template>
  <div class="expand-card" v-if="toolEvents.length">
    <div class="expand-card-header" @click="expanded = !expanded">
      <span class="icon">🔧</span>
      <span>Tool Trace</span>
      <span class="tag tag-warning" style="margin-left:6px;">{{ toolEvents.length }}</span>
      <span class="arrow" :class="{ open: expanded }">▼</span>
    </div>
    <div class="expand-card-body" v-if="expanded">
      <div class="trace-item" v-for="(evt, idx) in toolEvents" :key="idx">
        <div class="trace-status" :class="(evt.payload.status || '').toLowerCase()"></div>
        <div class="trace-info">
          <div style="display:flex;align-items:center;gap:8px;">
            <span class="trace-tool-name">{{ evt.payload.tool_name || 'tool' }}</span>
            <span class="tag" :class="{
              'tag-success': (evt.payload.status || '').toLowerCase() === 'completed',
              'tag-error': (evt.payload.status || '').toLowerCase() === 'failed',
              'tag-warning': (evt.payload.status || '').toLowerCase() === 'running'
            }">{{ evt.payload.status }}</span>
          </div>
          <div class="trace-result" v-if="evt.payload.result">{{ evt.payload.result }}</div>
          <div class="trace-result" style="color:var(--error)" v-if="evt.payload.error">{{ evt.payload.error }}</div>
        </div>
      </div>
    </div>
  </div>
</template>
