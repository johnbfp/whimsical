<script setup>
import { ref, computed, nextTick, watch } from 'vue'
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
const messages = ref([])
const sidebarOpen = ref(true)
const activePanel = ref(null) // 'model' | 'plugin' | 'memory' | 'events' | 'settings'
let socket = null

const chatArea = ref(null)

const isRunning = computed(() =>
  !['COMPLETED', 'FAILED', 'CANCELLED', 'IDLE'].includes(taskState.value)
)

const stateLabel = computed(() => {
  const s = taskState.value
  if (s === 'COMPLETED') return 'completed'
  if (s === 'FAILED' || s === 'CANCELLED') return 'failed'
  if (s === 'IDLE') return 'idle'
  return 'running'
})

function scrollToBottom() {
  nextTick(() => {
    if (chatArea.value) {
      chatArea.value.scrollTop = chatArea.value.scrollHeight
    }
  })
}

watch(messages, scrollToBottom, { deep: true })

function onTaskCreated(data, prompt) {
  taskId.value = data.task_id
  taskState.value = data.state
  taskResult.value = ''
  errorMsg.value = ''
  events.value = [{ event_type: 'task_event', payload: data, timestamp: new Date().toISOString() }]
  currentPlan.value = null

  // Add user message
  messages.value.push({
    role: 'user',
    text: prompt,
    timestamp: new Date().toISOString()
  })

  // Add agent "thinking" message
  messages.value.push({
    role: 'agent',
    text: '',
    thinking: true,
    taskId: data.task_id,
    state: data.state,
    timestamp: new Date().toISOString()
  })

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

    // Update the last agent message
    const lastAgent = [...messages.value].reverse().find(m => m.role === 'agent')
    if (lastAgent) {
      lastAgent.state = taskState.value
      lastAgent.thinking = isRunning.value
      if (event?.payload?.plan) lastAgent.plan = event.payload.plan
      if (event?.payload?.result) {
        lastAgent.text = event.payload.result
        lastAgent.thinking = false
      }
      if (event?.payload?.error) {
        lastAgent.error = event.payload.error
        lastAgent.thinking = false
      }
    }
  }
  socket.onerror = () => {
    errorMsg.value = 'WebSocket connection failed'
    const lastAgent = [...messages.value].reverse().find(m => m.role === 'agent')
    if (lastAgent) {
      lastAgent.error = 'WebSocket connection failed'
      lastAgent.thinking = false
    }
  }
}

async function cancelTask() {
  if (!taskId.value) return
  await fetch(`${apiBase.value}/agents/tasks/${taskId.value}/cancel`, { method: 'POST' })
}

function openPanel(panel) {
  activePanel.value = activePanel.value === panel ? null : panel
}

function closePanel() {
  activePanel.value = null
}

const quickPrompts = [
  'Build a production-grade REST API',
  'Analyze and optimize this codebase',
  'Create a CI/CD pipeline for deployment',
  'Design a microservice architecture'
]
</script>

<template>
  <div class="app-layout">
    <!-- Sidebar -->
    <aside class="sidebar" :class="{ open: sidebarOpen }">
      <div class="sidebar-header">
        <div class="sidebar-brand">
          <div class="brand-icon">W</div>
          <div>
            <div class="brand-text">Whimsical Agent</div>
            <div class="brand-sub">AI Runtime Console</div>
          </div>
        </div>
        <button class="new-task-btn" @click="messages = []; taskId = ''; taskState = 'IDLE'; taskResult = ''; errorMsg = ''; events = []; currentPlan = null">
          <span>+</span> New conversation
        </button>
      </div>

      <div class="sidebar-body">
        <div class="sidebar-section">
          <div class="sidebar-section-title">Controls</div>
          <div class="sidebar-item" :class="{ active: activePanel === 'model' }" @click="openPanel('model')">
            <span class="sidebar-item-icon">⚡</span>
            <span>Model Switch</span>
          </div>
          <div class="sidebar-item" :class="{ active: activePanel === 'plugin' }" @click="openPanel('plugin')">
            <span class="sidebar-item-icon">🧩</span>
            <span>Plugin Manager</span>
          </div>
          <div class="sidebar-item" :class="{ active: activePanel === 'memory' }" @click="openPanel('memory')">
            <span class="sidebar-item-icon">🧠</span>
            <span>Memory Recall</span>
          </div>
        </div>

        <div class="sidebar-section">
          <div class="sidebar-section-title">Monitor</div>
          <div class="sidebar-item" :class="{ active: activePanel === 'events' }" @click="openPanel('events')">
            <span class="sidebar-item-icon">📋</span>
            <span>Event Log</span>
            <span v-if="events.length" style="margin-left:auto;font-size:11px;color:var(--text-muted)">{{ events.length }}</span>
          </div>
        </div>

        <div class="sidebar-section" v-if="taskId">
          <div class="sidebar-section-title">Current Task</div>
          <div style="padding: 8px 10px;">
            <div style="font-size: 12px; color: var(--text-muted); margin-bottom: 6px;">
              {{ taskId.slice(0, 12) }}…
            </div>
            <span class="state-pill" :class="stateLabel">
              <span class="state-dot"></span>
              {{ taskState }}
            </span>
            <div style="margin-top: 8px;" v-if="isRunning">
              <button class="btn btn-ghost btn-sm" @click="cancelTask" style="width:100%">Cancel Task</button>
            </div>
          </div>
        </div>
      </div>

      <div class="sidebar-footer">
        <div class="sidebar-footer-info">
          <div class="user-avatar">{{ userId.charAt(0).toUpperCase() }}</div>
          <div>
            <div style="color:var(--sidebar-text);font-size:13px;">{{ userId }}</div>
            <div style="font-size:11px;">Python + Rust Engine</div>
          </div>
        </div>
      </div>
    </aside>

    <!-- Main Chat Area -->
    <main class="main-area">
      <!-- Header -->
      <div class="main-header">
        <div class="main-header-left">
          <button class="header-btn" @click="sidebarOpen = !sidebarOpen" style="display:none">☰</button>
          <div class="model-selector" @click="openPanel('model')">
            Whimsical Agent
            <svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor"><path d="M8 11L3 6h10z"/></svg>
          </div>
        </div>
        <div class="main-header-right">
          <span v-if="taskId" class="state-pill" :class="stateLabel" style="font-size:11px; padding:3px 10px;">
            <span class="state-dot"></span>
            {{ taskState }}
          </span>
        </div>
      </div>

      <!-- Chat area -->
      <div class="chat-area" ref="chatArea">
        <!-- Welcome screen -->
        <div class="welcome-screen" v-if="!messages.length">
          <div class="welcome-icon">W</div>
          <h1 class="welcome-title">Whimsical Agent</h1>
          <p class="welcome-sub">
            AI-powered runtime with Python orchestration and Rust execution. 
            Create tasks, manage plugins, switch models, and explore memory — all in one place.
          </p>
          <div class="welcome-chips">
            <div
              class="welcome-chip"
              v-for="(p, i) in quickPrompts"
              :key="i"
              @click="$refs.taskPanel.setPrompt(p)"
            >
              {{ p }}
            </div>
          </div>
        </div>

        <!-- Messages -->
        <div class="chat-messages" v-else>
          <div
            class="message"
            v-for="(msg, idx) in messages"
            :key="idx"
          >
            <div class="message-wrapper">
              <div class="message-avatar" :class="msg.role">
                {{ msg.role === 'user' ? userId.charAt(0).toUpperCase() : 'W' }}
              </div>
              <div class="message-content">
                <div class="message-role">
                  {{ msg.role === 'user' ? userId : 'Whimsical Agent' }}
                </div>

                <!-- Thinking indicator -->
                <div v-if="msg.thinking && !msg.text" class="typing-dots">
                  <span></span><span></span><span></span>
                </div>

                <!-- Message text -->
                <div class="message-text" v-if="msg.text">
                  <pre v-if="msg.text.length > 100">{{ msg.text }}</pre>
                  <template v-else>{{ msg.text }}</template>
                </div>

                <!-- Error -->
                <div class="inline-error" v-if="msg.error">
                  <strong>Error:</strong> {{ msg.error }}
                </div>

                <!-- Plan (inline expandable) -->
                <PlanView :plan="msg.plan" v-if="msg.plan" />

                <!-- Tool Trace (show for agent messages when running) -->
                <ToolTrace :events="events" v-if="msg.role === 'agent' && idx === messages.length - 1" />
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- Input Area (sticky bottom) -->
      <div class="input-area">
        <TaskPanel
          ref="taskPanel"
          :apiBase="apiBase"
          :userId="userId"
          :disabled="isRunning"
          @task-created="onTaskCreated"
        />
      </div>
    </main>

    <!-- Right side panels -->
    <Transition name="fade">
      <div class="right-panel-overlay" v-if="activePanel" @click="closePanel"></div>
    </Transition>
    <Transition name="slide">
      <div class="right-panel" v-if="activePanel">
        <div class="right-panel-header">
          <h3>
            {{ activePanel === 'model' ? 'Model Switch' : '' }}
            {{ activePanel === 'plugin' ? 'Plugin Manager' : '' }}
            {{ activePanel === 'memory' ? 'Memory Recall' : '' }}
            {{ activePanel === 'events' ? 'Event Log' : '' }}
            {{ activePanel === 'settings' ? 'Settings' : '' }}
          </h3>
          <button class="right-panel-close" @click="closePanel">✕</button>
        </div>
        <div class="right-panel-body">
          <ModelSwitch :apiBase="apiBase" v-if="activePanel === 'model'" />
          <PluginPanel :apiBase="apiBase" v-if="activePanel === 'plugin'" />
          <MemoryRecall :apiBase="apiBase" v-if="activePanel === 'memory'" />
          <EventLog :events="events" v-if="activePanel === 'events'" />
          <div v-if="activePanel === 'settings'">
            <div class="form-group">
              <label class="form-label">API Base URL</label>
              <input class="form-input" v-model="apiBase" />
            </div>
            <div class="form-group">
              <label class="form-label">User ID</label>
              <input class="form-input" v-model="userId" />
            </div>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>
