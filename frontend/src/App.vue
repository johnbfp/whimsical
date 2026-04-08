<script setup>
import { ref, computed, nextTick, watch, onMounted } from 'vue'
import TaskPanel from './components/TaskPanel.vue'
import PlanView from './components/PlanView.vue'
import ToolTrace from './components/ToolTrace.vue'
import EventLog from './components/EventLog.vue'
import ModelSwitch from './components/ModelSwitch.vue'
import PluginPanel from './components/PluginPanel.vue'
import MemoryRecall from './components/MemoryRecall.vue'
import WorkspacePanel from './components/WorkspacePanel.vue'

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
const activePanel = ref(null) // 'model' | 'plugin' | 'memory' | 'events' | 'settings' | 'workspace'
const activeSidebarSection = ref('controls')
let socket = null

const chatArea = ref(null)

// ── Session management ────────────────────────────────────
const STORAGE_KEY = 'whimsical_sessions'
const sessions = ref([])         // [{id, title, messages, taskId, taskState, events, createdAt, updatedAt}]
const currentSessionId = ref(null)

function genId() {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 7)
}

function sessionTitle(session) {
  const first = session.messages.find(m => m.role === 'user')
  return first ? first.text.slice(0, 36) : '新会话'
}

function formatTime(iso) {
  if (!iso) return ''
  const d = new Date(iso)
  const diff = Date.now() - d
  if (diff < 60000) return '刚刚'
  if (diff < 3600000) return `${Math.floor(diff / 60000)}分钟前`
  if (diff < 86400000) return `${Math.floor(diff / 3600000)}小时前`
  return `${d.getMonth() + 1}/${d.getDate()}`
}

function saveSessions() {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(sessions.value)) } catch {}
}

function saveCurrentSession() {
  if (!currentSessionId.value) return
  const idx = sessions.value.findIndex(s => s.id === currentSessionId.value)
  if (idx === -1) return
  sessions.value[idx] = {
    ...sessions.value[idx],
    title: sessionTitle(sessions.value[idx]),
    messages: JSON.parse(JSON.stringify(messages.value)),
    taskId: taskId.value,
    taskState: taskState.value,
    events: events.value.slice(0, 200),
    updatedAt: new Date().toISOString()
  }
  saveSessions()
}

function _applySession(session) {
  currentSessionId.value = session.id
  messages.value = JSON.parse(JSON.stringify(session.messages))
  taskId.value = session.taskId || ''
  taskState.value = session.taskState || 'IDLE'
  events.value = session.events || []
  currentPlan.value = null
  errorMsg.value = ''
  taskResult.value = ''
}

function newSession() {
  saveCurrentSession()
  const session = {
    id: genId(),
    title: '新会话',
    messages: [],
    taskId: '',
    taskState: 'IDLE',
    events: [],
    createdAt: new Date().toISOString(),
    updatedAt: new Date().toISOString()
  }
  sessions.value.unshift(session)
  saveSessions()
  _applySession(session)
}

function loadSession(id) {
  if (id === currentSessionId.value) return
  saveCurrentSession()
  const session = sessions.value.find(s => s.id === id)
  if (session) _applySession(session)
}

function deleteSession(id) {
  const idx = sessions.value.findIndex(s => s.id === id)
  if (idx === -1) return
  sessions.value.splice(idx, 1)
  saveSessions()
  if (currentSessionId.value === id) {
    if (sessions.value.length > 0) {
      _applySession(sessions.value[0])
    } else {
      newSession()
    }
  }
}

onMounted(() => {
  try {
    const stored = localStorage.getItem(STORAGE_KEY)
    if (stored) {
      sessions.value = JSON.parse(stored)
    }
  } catch {}
  if (sessions.value.length > 0) {
    _applySession(sessions.value[0])
  } else {
    newSession()
  }
})

// Auto-save when messages change
watch(messages, () => { saveCurrentSession() }, { deep: true })

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

function toggleSidebarSection(section) {
  activeSidebarSection.value = activeSidebarSection.value === section ? null : section
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
    <!-- Mobile overlay: tap outside sidebar to close it -->
    <div
      v-if="sidebarOpen"
      class="sidebar-overlay"
      @click="sidebarOpen = false"
    ></div>
    <aside class="sidebar" :class="{ open: sidebarOpen, collapsed: !sidebarOpen }">
      <div class="sidebar-header">
        <div class="sidebar-brand">
          <div class="brand-icon">W</div>
          <div>
            <div class="brand-text">Whimsical Agent</div>
            <div class="brand-sub">AI Runtime Console</div>
          </div>
        </div>
        <button class="new-task-btn" @click="newSession()">
          <span>+</span> New conversation
        </button>
      </div>

      <div class="sidebar-body">
        <!-- Sessions history list -->
        <div class="sessions-list">
          <div class="sidebar-section-title" style="padding: 10px 10px 4px; display:block;">会话记录</div>
          <div
            v-for="session in sessions"
            :key="session.id"
            class="session-item"
            :class="{ active: session.id === currentSessionId }"
            @click="loadSession(session.id)"
          >
            <div class="session-item-body">
              <div class="session-title">{{ sessionTitle(session) }}</div>
              <div class="session-time">{{ formatTime(session.updatedAt) }}</div>
            </div>
            <button class="session-delete" @click.stop="deleteSession(session.id)" title="删除">×</button>
          </div>
          <div v-if="!sessions.length" class="session-empty">暂无会话</div>
        </div>

        <div class="sidebar-divider"></div>
        <div class="sidebar-section">
          <button
            class="sidebar-section-toggle"
            :class="{ open: activeSidebarSection === 'controls' }"
            @click="toggleSidebarSection('controls')"
          >
            <span class="sidebar-section-title">Controls</span>
            <span class="sidebar-section-arrow">▾</span>
          </button>
          <div class="sidebar-section-content" v-if="activeSidebarSection === 'controls'">
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
              <span>Memory</span>
            </div>
            <div class="sidebar-item" :class="{ active: activePanel === 'workspace' }" @click="openPanel('workspace')">
              <span class="sidebar-item-icon">💻</span>
              <span>Workspace</span>
            </div>
          </div>
        </div>

        <div class="sidebar-section">
          <button
            class="sidebar-section-toggle"
            :class="{ open: activeSidebarSection === 'monitor' }"
            @click="toggleSidebarSection('monitor')"
          >
            <span class="sidebar-section-title">Monitor</span>
            <span class="sidebar-section-arrow">▾</span>
          </button>
          <div class="sidebar-section-content" v-if="activeSidebarSection === 'monitor'">
            <div class="sidebar-item" :class="{ active: activePanel === 'events' }" @click="openPanel('events')">
              <span class="sidebar-item-icon">📋</span>
              <span>Event Log</span>
              <span v-if="events.length" style="margin-left:auto;font-size:11px;color:var(--text-muted)">{{ events.length }}</span>
            </div>
          </div>
        </div>

        <div class="sidebar-section" v-if="taskId">
          <button
            class="sidebar-section-toggle"
            :class="{ open: activeSidebarSection === 'task' }"
            @click="toggleSidebarSection('task')"
          >
            <span class="sidebar-section-title">Current Task</span>
            <span class="sidebar-section-arrow">▾</span>
          </button>
          <div class="sidebar-section-content" v-if="activeSidebarSection === 'task'">
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
          <button class="header-btn" @click="sidebarOpen = !sidebarOpen" :title="sidebarOpen ? 'Collapse sidebar' : 'Expand sidebar'">☰</button>
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
      <div class="right-panel" :class="{ wide: activePanel === 'workspace' }" v-if="activePanel">
        <div class="right-panel-header">
          <h3>
            {{ activePanel === 'model' ? 'Model Switch' : '' }}
            {{ activePanel === 'plugin' ? 'Plugin Manager' : '' }}
            {{ activePanel === 'memory' ? 'Memory' : '' }}
            {{ activePanel === 'events' ? 'Event Log' : '' }}
            {{ activePanel === 'settings' ? 'Settings' : '' }}
            {{ activePanel === 'workspace' ? 'Workspace' : '' }}
          </h3>
          <button class="right-panel-close" @click="closePanel">✕</button>
        </div>
        <div class="right-panel-body">
          <ModelSwitch :apiBase="apiBase" v-if="activePanel === 'model'" />
          <PluginPanel :apiBase="apiBase" v-if="activePanel === 'plugin'" />
          <MemoryRecall :apiBase="apiBase" v-if="activePanel === 'memory'" />
          <EventLog :events="events" v-if="activePanel === 'events'" />
          <WorkspacePanel :apiBase="apiBase" v-if="activePanel === 'workspace'" />
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
