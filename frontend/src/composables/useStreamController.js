import { ref } from 'vue'
import { apiGet } from '../api.js'

// framesize enum → [width, height] (from esp32-camera sensor.h)
const FRAME_DIMS = [
  [96, 96], [160, 120], [128, 128], [176, 144], [240, 176], [240, 240],
  [320, 240], [320, 320], [400, 296], [480, 320], [640, 480], [800, 600],
  [1024, 768], [1280, 720], [1280, 1024], [1600, 1200], [1920, 1080],
  [720, 1280], [864, 1536], [2048, 1536], [2560, 1440], [2560, 1600],
  [1080, 1920], [2560, 1920], [2592, 1944],
]

// Module-level singleton state
const playing = ref(false)
const hasCamera = ref(true)
const hasMic = ref(true)
const camWidth = ref(640)
const camHeight = ref(480)
const hwWarning = ref('')
const hostname = ref('chute')

let vUrl = ''
let aUrl = ''
let vidEl = null
let audEl = null
let initialized = false

async function init() {
  if (initialized) return
  initialized = true
  try {
    const info = await apiGet('/api/info')
    const host = location.hostname
    vUrl = 'http://' + host + ':' + info.stream_port + '/stream'
    aUrl = 'http://' + host + ':' + info.audio_port + '/audio'
    hasCamera.value = info.camera !== false
    hasMic.value = info.mic !== false
    hostname.value = info.hostname || 'chute'

    if (!hasCamera.value && !hasMic.value) hwWarning.value = 'Camera and microphone not detected.'
    else if (!hasCamera.value) hwWarning.value = 'Camera not detected. Only audio streaming is available.'
    else if (!hasMic.value) hwWarning.value = 'Microphone not detected. Only video streaming is available.'

    if (hasCamera.value) {
      try {
        const st = await apiGet('/api/camera/status')
        const dims = FRAME_DIMS[st.framesize]
        if (dims) { camWidth.value = dims[0]; camHeight.value = dims[1] }
      } catch {}
    }
  } catch (e) {
    console.error('Failed to load stream info', e)
  }
}

function registerElements(vid, aud) {
  vidEl = vid
  audEl = aud
}

function unregisterElements() {
  stop()
  vidEl = null
  audEl = null
}

function stop() {
  if (vidEl?.value) vidEl.value.src = ''
  if (audEl?.value) { audEl.value.pause(); audEl.value.src = '' }
  playing.value = false
}

function play() {
  if (hasCamera.value && vidEl?.value) vidEl.value.src = vUrl
  if (hasMic.value && audEl?.value) { audEl.value.src = aUrl; audEl.value.play() }
  playing.value = true
}

function togglePlay() {
  playing.value ? stop() : play()
}

function restartVideo() {
  if (!playing.value || !vidEl?.value) return
  vidEl.value.src = ''
  setTimeout(() => { if (playing.value && vidEl?.value) vidEl.value.src = vUrl }, 300)
}

function restartAudio() {
  if (!playing.value || !audEl?.value) return
  audEl.value.pause()
  audEl.value.src = ''
  setTimeout(() => {
    if (playing.value && audEl?.value) { audEl.value.src = aUrl; audEl.value.play() }
  }, 300)
}

function updateFrameDims(framesize) {
  const dims = FRAME_DIMS[framesize]
  if (dims) { camWidth.value = dims[0]; camHeight.value = dims[1] }
}

export function useStreamController() {
  return {
    playing, hasCamera, hasMic, camWidth, camHeight, hwWarning, hostname,
    init, registerElements, unregisterElements,
    play, stop, togglePlay,
    restartVideo, restartAudio, updateFrameDims,
  }
}
