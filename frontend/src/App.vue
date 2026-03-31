<script setup>
import { ref, computed } from 'vue'
import TaskPanel from './components/TaskPanel.vue'
import PlanView from './components/PlanView.vue'
import ToolTrace from './components/ToolTrace.vue'
import EventLog from './components/EventLog.vue'
import ModelSwitch from './components/ModelSwitch.vue'
import PluginPanel from './components/PluginPanel.vue'
import MemoryRecall from './components/MemoryRecall.vue'

const apiBase = ref(window.location.origin)
const userId = ref('demo-user')
const taskId = ref('')
const taskState = ref('IDLE')
const taskResult = ref('')
const events = ref([])
const currentPlan = ref(null)
const errorMsg = ref('')
let socket = null

const stateClass = computed(() => {
  const s = taskState.value
  if (s === 'COMPLETED') return 'state-ok'
  if (s === 'FAILED' || s === 'CANCELLED') return 'state-err'
  if (s === 'IDLE') return 'state-idle'
  return 'state-running'
})

function onTaskCreated(data) {
  taskId.value = data.task_id
  taskState.value = data.state
  taskResult.value = ''
  errorMsg.value = ''
  events.value = [{ event_type: 'task_event', payload: data, timestamp: new Date().toISOString() }]
  currentPlan.value = null
  subscribe()
}

function subscribe() {
  if (!taskId.value) return
  if (socket) socket.close()
  const wsBase = apiBase.value.replace(/^http/, 'ws')
  socket = new WebSocket(`${wsBase}/agents/stream?task_id=${taskId.value}`)
  socket.onmessage = (evt) => {
    const event = JSON.parse(evt.data)
    events.value.unshift(event)
    if (event?.payload?.state) taskState.value = event.payload.state
    if (event?.payload?.plan) currentPlan.value = event.payload.plan
    if (event?.payload?.result) taskResult.value = event.payload.result
    if (event?.payload?.error) errorMsg.value = event.payload.error
  }
  socket.onerror = () => { errorMsg.value = 'WebSocket connection failed' }
}

async function cancelTask() {
  if (!taskId.value) return
  await fetch(`${apiBase.value}/agents/tasks/${taskId.value}/cancel`, { method: 'POST' })
}

async function pollTask() {
  if (!taskId.value) return
  const resp = await fetch(`${apiBase.value}/agents/tasks/${taskId.value}`)
  if (resp.ok) {
    const data = await resp.json()
    taskState.value = data.state
    if (data.result) taskResult.value = data.result
    if (data.plan) currentPlan.value = data.plan
  }
}
</script>

<template>
  <div class="container">
    <div class="panel">
      <h2>Agent Runtime Console</h2>
      <p class="muted">Python Orchestrator + Rust Executor + WebSocket Live Stream</p>

      <!-- Config bar -->
      <div class="row">
        <input v-model="apiBase" style="min-width:280px" />
        <input v-model="userId" placeholder="user id" />
      </div>

      <!-- State bar -->
      <div class="state-bar" v-if="taskId">
        <span class="muted">task: {{ taskId.slice(0, 8) }}…</span>
        <span class="state-badge" :class="stateClass">{{ taskState }}</span>
        <button class="small" @click="cancelTask" v-if="!['COMPLETED','FAILED','CANCELLED','IDLE'].includes(taskState)">Cancel</button>
        <button class="small" @click="pollTask">Refresh</button>
      </div>

      <!-- Task result -->
      <div class="result-box" v-if="taskResult">
        <h3>Result</h3>
        <pre>{{ taskResult }}</pre>
      </div>

      <!-- Error display -->
      <div class="error-box" v-if="errorMsg">
        <strong>Error:</strong> {{ errorMsg }}
      </div>

      <!-- Task creation -->
      <TaskPanel :apiBase="apiBase" :userId="userId" @task-created="onTaskCreated" />

      <!-- Plan visualization -->
      <PlanView :plan="currentPlan" />

      <!-- Tool execution trace -->
      <ToolTrace :events="events" />

      <!-- Full event log -->
      <EventLog :events="events" />

      <!-- Collapsible controls -->
      <details class="section-collapse">
        <summary>Model / Plugin / Memory</summary>
        <ModelSwitch :apiBase="apiBase" />
        <PluginPanel :apiBase="apiBase" />
        <MemoryRecall :apiBase="apiBase" />
      </details>
    </div>
  </div>
</template>
