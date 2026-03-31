<script setup>
import { defineProps } from 'vue'

const props = defineProps({
  plan: Object // Plan | null
})
</script>

<template>
  <div class="section" v-if="plan">
    <h3>Execution Plan</h3>
    <div class="plan-breakdown">
      <h4>Task Breakdown</h4>
      <ol>
        <li v-for="(step, i) in plan.task_breakdown" :key="i">{{ step }}</li>
      </ol>
    </div>
    <div class="plan-tools">
      <h4>Selected Tools</h4>
      <span class="badge" v-for="tool in plan.selected_tools" :key="tool">{{ tool }}</span>
    </div>
    <div class="plan-steps">
      <h4>Execution Steps</h4>
      <div class="step-card" v-for="(step, i) in plan.execution_steps" :key="i">
        <strong>{{ i + 1 }}. {{ step.tool_name }}</strong>
        <pre>{{ JSON.stringify(step.input, null, 2) }}</pre>
      </div>
    </div>
    <div class="plan-reflection" v-if="plan.reflection_hint">
      <h4>Reflection</h4>
      <p class="muted">{{ plan.reflection_hint }}</p>
    </div>
  </div>
</template>

<style scoped>
.plan-breakdown, .plan-tools, .plan-steps, .plan-reflection { margin-top: 8px; }
.badge {
  display: inline-block;
  background: var(--accent);
  color: #fff;
  padding: 2px 10px;
  border-radius: 12px;
  font-size: 0.85em;
  margin-right: 6px;
}
.step-card {
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  padding: 8px 12px;
  margin-top: 6px;
}
.step-card pre { margin: 4px 0 0; font-size: 0.85em; white-space: pre-wrap; }
</style>
