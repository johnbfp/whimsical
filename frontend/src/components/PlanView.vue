<script setup>
import { ref, defineProps } from 'vue'

const props = defineProps({
  plan: Object
})

const expanded = ref(false)
</script>

<template>
  <div class="expand-card" v-if="plan">
    <div class="expand-card-header" @click="expanded = !expanded">
      <span class="icon">📋</span>
      <span>Execution Plan</span>
      <div style="display:flex;gap:4px;margin-left:8px;" v-if="plan.selected_tools">
        <span class="tag tag-accent" v-for="tool in plan.selected_tools.slice(0,3)" :key="tool">{{ tool }}</span>
        <span class="tag tag-accent" v-if="plan.selected_tools.length > 3">+{{ plan.selected_tools.length - 3 }}</span>
      </div>
      <span class="arrow" :class="{ open: expanded }">▼</span>
    </div>
    <div class="expand-card-body" v-if="expanded">
      <!-- Task Breakdown -->
      <div v-if="plan.task_breakdown && plan.task_breakdown.length" style="margin-bottom:12px;">
        <div style="font-size:11px;text-transform:uppercase;color:var(--text-muted);font-weight:600;margin-bottom:6px;">Task Breakdown</div>
        <ol class="plan-steps-list">
          <li class="plan-step" v-for="(step, i) in plan.task_breakdown" :key="i">
            <span class="step-num">{{ i + 1 }}</span>
            <span class="step-text">{{ step }}</span>
          </li>
        </ol>
      </div>

      <!-- Execution Steps -->
      <div v-if="plan.execution_steps && plan.execution_steps.length" style="margin-bottom:12px;">
        <div style="font-size:11px;text-transform:uppercase;color:var(--text-muted);font-weight:600;margin-bottom:6px;">Execution Steps</div>
        <div v-for="(step, i) in plan.execution_steps" :key="i" style="margin-bottom:8px;">
          <div style="display:flex;align-items:center;gap:6px;">
            <span class="step-num">{{ i + 1 }}</span>
            <span class="tag tag-info">{{ step.tool_name }}</span>
          </div>
          <pre v-if="step.input">{{ JSON.stringify(step.input, null, 2) }}</pre>
        </div>
      </div>

      <!-- Reflection -->
      <div v-if="plan.reflection_hint" style="padding:8px 12px;background:var(--info-bg);border-radius:var(--radius-sm);color:var(--text-secondary);font-size:13px;">
        💡 {{ plan.reflection_hint }}
      </div>
    </div>
  </div>
</template>
