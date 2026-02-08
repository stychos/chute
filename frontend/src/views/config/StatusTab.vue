<template>
  <div class="space-y-3">
    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Network</h2>
      <div class="grid grid-cols-2 gap-2 text-sm">
        <span class="text-text-dim">IP Address</span><span>{{ data.ip || '...' }}</span>
        <span class="text-text-dim">WiFi Mode</span><span>{{ data.wifi_mode || '...' }}</span>
        <span class="text-text-dim">SSID</span><span>{{ data.ssid || '...' }}</span>
        <span class="text-text-dim">RSSI</span><span>{{ data.rssi != null ? data.rssi + ' dBm' : '...' }}</span>
        <span class="text-text-dim">WiFi Preference</span><span>{{ data.wifi_mode_pref || '...' }}</span>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">System</h2>
      <div class="grid grid-cols-2 gap-2 text-sm">
        <span class="text-text-dim">Free Heap</span><span>{{ data.free_heap ? formatBytes(data.free_heap) : '...' }}</span>
        <span class="text-text-dim">Min Free Heap</span><span>{{ data.min_free_heap ? formatBytes(data.min_free_heap) : '...' }}</span>
        <span class="text-text-dim">PSRAM Free</span><span>{{ data.psram_free ? formatBytes(data.psram_free) : '...' }}</span>
        <span class="text-text-dim">SPIFFS</span><span>{{ data.spiffs_used != null ? formatBytes(data.spiffs_used) + ' / ' + formatBytes(data.spiffs_total) : '...' }}</span>
        <span class="text-text-dim">Chip</span><span>{{ data.chip || '...' }}</span>
        <span class="text-text-dim">Uptime</span><span>{{ data.uptime_s != null ? formatUptime(data.uptime_s) : '...' }}</span>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Camera</h2>
      <div class="grid grid-cols-2 gap-2 text-sm">
        <span class="text-text-dim">Sensor</span><span>{{ camera.sensor || '...' }}</span>
        <span class="text-text-dim">Resolution</span><span>{{ camera.resolution || '...' }}</span>
        <span class="text-text-dim">JPEG Quality</span><span>{{ camera.quality != null ? camera.quality : '...' }}</span>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Audio</h2>
      <div class="grid grid-cols-2 gap-2 text-sm">
        <span class="text-text-dim">Sample Rate</span><span>{{ audio.sample_rate ? audio.sample_rate + ' Hz' : '...' }}</span>
        <span class="text-text-dim">Mic Bit Depth</span><span>{{ audio.mic_bits ? audio.mic_bits + '-bit' : '...' }}</span>
        <span class="text-text-dim">WAV Bit Depth</span><span>{{ audio.wav_bits ? audio.wav_bits + '-bit' : '...' }}</span>
        <span class="text-text-dim">Mic Gain</span><span>{{ audio.mic_gain != null ? audio.mic_gain + 'x' : '...' }}</span>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Partitions</h2>
      <div class="grid grid-cols-2 gap-2 text-sm">
        <span class="text-text-dim">Running</span><span>{{ data.running_partition || '...' }}</span>
        <span class="text-text-dim">Boot</span><span>{{ data.boot_partition || '...' }}</span>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { apiGet } from '../../api.js'

const data = ref({})
const camera = ref({})
const audio = ref({})
let timer = null

const SENSOR_NAMES = { 0x26: 'OV2640', 0x3660: 'OV3660', 0x5640: 'OV5640' }
const FRAME_SIZES = [
  '96x96', '160x120', '176x144', '240x176', '240x240',
  '320x240', '400x296', '480x320', '640x480', '800x600',
  '1024x768', '1280x720', '1280x1024', '1600x1200'
]

function formatBytes(b) {
  if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB'
  if (b >= 1024) return (b / 1024).toFixed(0) + ' KB'
  return b + ' B'
}

function formatUptime(s) {
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  if (d > 0) return d + 'd ' + h + 'h ' + m + 'm'
  if (h > 0) return h + 'h ' + m + 'm'
  return m + 'm ' + (s % 60) + 's'
}

async function poll() {
  try {
    data.value = await apiGet('/api/system/info')
  } catch (e) {
    console.error('Poll failed', e)
  }
}

async function fetchCamera() {
  try {
    const [info, status] = await Promise.all([
      apiGet('/api/camera/info'),
      apiGet('/api/camera/status')
    ])
    camera.value = {
      sensor: SENSOR_NAMES[info.pid] || ('PID 0x' + info.pid.toString(16)),
      resolution: FRAME_SIZES[status.framesize] || ('framesize ' + status.framesize),
      quality: status.quality
    }
  } catch (e) {
    console.error('Camera info failed', e)
  }
}

async function fetchAudio() {
  try {
    audio.value = await apiGet('/api/audio/config')
  } catch (e) {
    console.error('Audio info failed', e)
  }
}

onMounted(() => {
  poll()
  fetchCamera()
  fetchAudio()
  timer = setInterval(poll, 5000)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>
