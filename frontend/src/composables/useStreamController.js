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
const volume = ref(0.8)
const snapshotUrl = ref('')

let vUrl = ''
let aUrl = ''
let vidEl = null
let initialized = false
let snapshotInterval = null

// Web Audio API state
let audioCtx = null
let gainNode = null
let abortCtrl = null

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
      } catch (e) { console.warn('[video] camera status fetch failed', e) }
      fetchSnapshot()
      startSnapshotTimer()
    }
  } catch (e) {
    console.error('[stream] Failed to load stream info', e)
  }
}

function registerElements(vid) {
  vidEl = vid
}

function unregisterElements() {
  stop()
  stopSnapshotTimer()
  vidEl = null
}

// --- Web Audio streaming ---

async function playAudio() {
  if (!audioCtx || !aUrl) return

  // Ensure context is running (may be suspended by autoplay policy)
  if (audioCtx.state !== 'running') {
    await audioCtx.resume()
  }

  abortCtrl = new AbortController()
  try {
    const res = await fetch(aUrl, { signal: abortCtrl.signal })
    if (!res.ok || !res.body) return
    const reader = res.body.getReader()

    // Accumulate bytes until we have the 44-byte WAV header
    let pending = new Uint8Array(0)
    while (pending.length < 44) {
      const { done, value } = await reader.read()
      if (done) return
      if (pending.length === 0) {
        pending = value
      } else {
        const m = new Uint8Array(pending.length + value.length)
        m.set(pending); m.set(value, pending.length)
        pending = m
      }
    }

    // Parse WAV header (little-endian)
    const bitsPerSample = pending[34] | (pending[35] << 8)
    const bytesPerSample = bitsPerSample / 8
    const wavSampleRate = (pending[24] | (pending[25] << 8) | (pending[26] << 16) | (pending[27] << 24)) >>> 0

    // Strip header, keep any extra audio bytes
    pending = pending.slice(44)

    let schedTime = audioCtx.currentTime

    // Main decode + schedule loop
    while (true) {
      // Process complete samples from pending buffer
      const usable = pending.length - (pending.length % bytesPerSample)
      if (usable > 0) {
        const numSamples = usable / bytesPerSample
        const floats = new Float32Array(numSamples)

        if (bitsPerSample === 16) {
          for (let i = 0; i < numSamples; i++) {
            const o = i * 2
            let v = pending[o] | (pending[o + 1] << 8)
            if (v >= 32768) v -= 65536
            floats[i] = v / 32768
          }
        } else if (bitsPerSample === 24) {
          for (let i = 0; i < numSamples; i++) {
            const o = i * 3
            let v = pending[o] | (pending[o + 1] << 8) | (pending[o + 2] << 16)
            if (v & 0x800000) v -= 0x1000000
            floats[i] = v / 8388608
          }
        }

        pending = pending.slice(usable)

        // Schedule playback — catch errors if context was closed during await
        if (!audioCtx || audioCtx.state === 'closed') break
        const buf = audioCtx.createBuffer(1, numSamples, wavSampleRate)
        buf.getChannelData(0).set(floats)
        const src = audioCtx.createBufferSource()
        src.buffer = buf
        src.connect(gainNode)
        const now = audioCtx.currentTime
        if (schedTime < now) schedTime = now
        src.start(schedTime)
        schedTime += buf.duration
      }

      // Read next chunk from network
      const { done, value } = await reader.read()
      if (done) break
      if (pending.length > 0) {
        const m = new Uint8Array(pending.length + value.length)
        m.set(pending); m.set(value, pending.length)
        pending = m
      } else {
        pending = value
      }
    }
  } catch (e) {
    if (e.name !== 'AbortError') console.error('[audio] stream error', e)
  }
}

function stopAudio() {
  if (abortCtrl) { abortCtrl.abort(); abortCtrl = null }
  if (audioCtx && audioCtx.state !== 'closed') audioCtx.close().catch(() => {})
  audioCtx = null
  gainNode = null
}

// --- Public API ---

function fetchSnapshot() {
  if (!hasCamera.value) return
  snapshotUrl.value = '/api/camera/capture?t=' + Date.now()
}

function startSnapshotTimer() {
  stopSnapshotTimer()
  snapshotInterval = setInterval(fetchSnapshot, 30000)
}

function stopSnapshotTimer() {
  if (snapshotInterval) { clearInterval(snapshotInterval); snapshotInterval = null }
}

function stop() {
  if (vidEl?.value) vidEl.value.src = ''
  stopAudio()
  playing.value = false
  fetchSnapshot()
  startSnapshotTimer()
}

function play() {
  stopSnapshotTimer()
  if (hasCamera.value && vidEl?.value) {
    vidEl.value.src = vUrl
  }
  if (hasMic.value && aUrl) {
    // Create AudioContext synchronously within user gesture for autoplay compliance
    audioCtx = new AudioContext()
    audioCtx.resume()
    gainNode = audioCtx.createGain()
    gainNode.gain.value = volume.value
    gainNode.connect(audioCtx.destination)
    playAudio()
  }
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
  if (!playing.value) return
  if (abortCtrl) { abortCtrl.abort(); abortCtrl = null }
  setTimeout(() => {
    if (!playing.value || !audioCtx) return
    playAudio()
  }, 300)
}

function updateFrameDims(framesize) {
  const dims = FRAME_DIMS[framesize]
  if (dims) { camWidth.value = dims[0]; camHeight.value = dims[1] }
  console.log('[video] camera status: framesize=%d, dims=%dx%d', framesize, camWidth.value, camHeight.value)
}

function setVolume(v) {
  volume.value = v
  if (gainNode) gainNode.gain.value = v
}

export function useStreamController() {
  return {
    playing, hasCamera, hasMic, camWidth, camHeight, hwWarning, hostname, volume, snapshotUrl,
    init, registerElements, unregisterElements,
    play, stop, togglePlay,
    restartVideo, restartAudio, updateFrameDims, setVolume,
  }
}
