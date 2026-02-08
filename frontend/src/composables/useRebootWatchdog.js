import { ref } from 'vue'
import { apiGet } from '../api.js'

const active = ref(false)
const status = ref('waiting')  // 'waiting' | 'online' | 'timeout'
const apSsid = ref('')
const apIp = '192.168.4.1'
let timer = null
let startTime = 0

const POLL_INTERVAL = 4000
const TIMEOUT = 60000

function stop() {
  clearInterval(timer)
  timer = null
  active.value = false
  status.value = 'waiting'
}

async function poll() {
  if (Date.now() - startTime > TIMEOUT) {
    status.value = 'timeout'
    clearInterval(timer)
    timer = null
    return
  }
  try {
    const info = await apiGet('/api/info')
    status.value = 'online'
    clearInterval(timer)
    timer = null
    // Auto-close after a moment so user sees "back online"
    setTimeout(() => { active.value = false }, 1500)
    // Reload page to refresh all data
    setTimeout(() => { location.reload() }, 1600)
  } catch {
    // still offline
  }
}

export function useRebootWatchdog() {
  function start(fallbackApSsid) {
    apSsid.value = fallbackApSsid || 'Chute-Setup'
    status.value = 'waiting'
    active.value = true
    startTime = Date.now()
    // Initial delay before first poll (give board time to actually go down)
    clearInterval(timer)
    timer = setInterval(poll, POLL_INTERVAL)
  }

  return { active, status, apSsid, apIp, start, stop }
}
