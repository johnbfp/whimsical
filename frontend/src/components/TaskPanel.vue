<script setup>
import { ref, defineEmits, defineProps, defineExpose, computed } from 'vue'

const props = defineProps({
  apiBase: String,
  userId: String,
  disabled: Boolean
})
const emit = defineEmits(['task-created'])

const prompt = ref('')
const loading = ref(false)
const textareaRef = ref(null)

const canSend = computed(() => prompt.value.trim().length > 0 && !loading.value && !props.disabled)

function setPrompt(text) {
  prompt.value = text
  if (textareaRef.value) textareaRef.value.focus()
}

defineExpose({ setPrompt })

function autoResize() {
  const el = textareaRef.value
  if (!el) return
  el.style.height = 'auto'
  el.style.height = Math.min(el.scrollHeight, 160) + 'px'
}

function handleKeydown(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault()
    if (canSend.value) createTask()
  }
}

async function createTask() {
  if (!canSend.value) return
  loading.value = true
  const taskPrompt = prompt.value.trim()
  prompt.value = ''
  if (textareaRef.value) {
    textareaRef.value.style.height = 'auto'
  }
  try {
    const resp = await fetch(`${props.apiBase}/agents/tasks`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ tenant_id: 'default', user_id: props.userId, prompt: taskPrompt })
    })
    const data = await resp.json()
    emit('task-created', data, taskPrompt)
  } catch (err) {
    // ignore
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="input-container">
    <div class="input-box">
      <textarea
        ref="textareaRef"
        class="input-textarea"
        v-model="prompt"
        :placeholder="disabled ? 'Agent is working...' : 'Send a message...'"
        :disabled="disabled"
        rows="1"
        @input="autoResize"
        @keydown="handleKeydown"
      ></textarea>
      <button
        class="send-btn"
        :class="{ active: canSend }"
        :disabled="!canSend"
        @click="createTask"
      >
        <svg width="16" height="16" viewBox="0 0 16 16" fill="none">
          <path d="M2 8L14 2L8 14L7 9L2 8Z" fill="currentColor"/>
        </svg>
      </button>
    </div>
    <div class="input-footer">
      Whimsical Agent — Python Orchestrator + Rust Executor
    </div>
  </div>
</template>
