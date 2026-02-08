<template>
  <div class="space-y-3">
    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">LED Flash</h2>

      <div class="space-y-3">
        <div class="flex items-end gap-3">
          <div class="flex-1">
            <label class="flex justify-between text-sm text-text-dim mb-1">
              <span>Intensity</span>
              <span class="text-text">{{ intensity }}</span>
            </label>
            <input type="range" min="0" max="255" v-model.number="intensity"
              @input="updateIntensity" class="w-full accent-accent">
          </div>
          <button @click="toggle"
            class="shrink-0 px-4 py-1.5 rounded text-sm font-medium transition-colors"
            :class="ledOn
              ? 'bg-accent hover:bg-accent-hover text-white'
              : 'bg-card border border-border hover:border-accent text-text-dim hover:text-accent'">
            {{ ledOn ? 'Turn Off' : 'Turn On' }}
          </button>
        </div>

        <label v-if="hasCamera" class="flex items-center gap-2 text-sm text-text-dim cursor-pointer">
          <input type="checkbox" v-model="streamEnabled" @change="updateStream" class="accent-accent">
          Enable LED on camera streaming
        </label>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { apiGet, apiPost } from '../../api.js'

const intensity = ref(0)
const ledOn = ref(false)
const streamEnabled = ref(true)
const hasCamera = ref(false)

onMounted(async () => {
  try {
    const [led, info] = await Promise.all([
      apiGet('/api/led/status'),
      apiGet('/api/info')
    ])
    intensity.value = led.intensity || 0
    ledOn.value = led.on || false
    streamEnabled.value = led.stream_enabled !== false
    hasCamera.value = info.camera !== false
  } catch (e) {
    console.error(e)
  }
})

function updateIntensity() {
  apiPost('/api/led/control', { intensity: intensity.value })
}

function toggle() {
  ledOn.value = !ledOn.value
  apiPost('/api/led/control', { on: ledOn.value ? 1 : 0 })
}

function updateStream() {
  apiPost('/api/led/control', { stream_enabled: streamEnabled.value ? 1 : 0 })
}
</script>
