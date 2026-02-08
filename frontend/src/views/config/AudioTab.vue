<template>
  <div class="bg-card rounded-lg p-4">
    <h2 class="text-accent text-sm font-semibold mb-3">Microphone</h2>

    <label class="block text-sm text-text-dim mb-1">Gain: {{ gain }}</label>
    <input type="range" v-model.number="gain" min="1" max="32" @input="onGainChange"
      class="w-full accent-accent mb-4">

    <label class="block text-sm text-text-dim mb-1">Sample Rate</label>
    <select v-model.number="sampleRate" class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">
      <option :value="8000">8000 Hz</option>
      <option :value="11025">11025 Hz</option>
      <option :value="16000">16000 Hz</option>
      <option :value="22050">22050 Hz</option>
      <option :value="44100">44100 Hz</option>
    </select>

    <label class="block text-sm text-text-dim mb-1">WAV Bit Depth</label>
    <select v-model.number="wavBits" class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-4">
      <option :value="16">16-bit</option>
      <option :value="24">24-bit</option>
    </select>

    <button @click="saveAudioConfig" class="bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors">
      Save
    </button>

    <p v-if="msg" class="text-xs mt-2" :class="msgErr ? 'text-red-400' : 'text-green-400'">{{ msg }}</p>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { apiGet, apiPost } from '../../api.js'
import { useStreamController } from '../../composables/useStreamController.js'

const stream = useStreamController()

const gain = ref(8)
const sampleRate = ref(22050)
const wavBits = ref(16)
const msg = ref('')
const msgErr = ref(false)
let debounceTimer = null

onMounted(async () => {
  try {
    const c = await apiGet('/api/audio/config')
    gain.value = c.mic_gain || 8
    sampleRate.value = c.sample_rate || 22050
    wavBits.value = c.wav_bits || 16
  } catch (e) {
    console.error(e)
  }
})

function onGainChange() {
  clearTimeout(debounceTimer)
  debounceTimer = setTimeout(async () => {
    try {
      await apiPost('/api/audio/config', { mic_gain: gain.value })
      msg.value = 'Gain saved'
      msgErr.value = false
      setTimeout(() => msg.value = '', 2000)
    } catch (e) {
      console.error(e)
    }
  }, 300)
}

async function saveAudioConfig() {
  msg.value = ''
  try {
    await apiPost('/api/audio/config', {
      mic_gain: gain.value,
      sample_rate: sampleRate.value,
      wav_bits: wavBits.value
    })
    msg.value = 'Saved'
    msgErr.value = false
    stream.restartAudio()
    setTimeout(() => msg.value = '', 2000)
  } catch (e) {
    msg.value = 'Save failed'
    msgErr.value = true
  }
}
</script>
