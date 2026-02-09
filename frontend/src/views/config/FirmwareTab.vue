<template>
  <div class="space-y-3">
    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">System</h2>

      <div class="space-y-4">
        <div class="flex items-center justify-between">
          <span class="text-sm text-text-dim">Boot Partition:</span>
          <button @click="switchBoot" class="bg-card border border-red-600 hover:border-red-500 text-red-400 hover:text-red-300 px-4 py-1.5 rounded text-sm transition-colors">
            {{ info.boot_partition || '...' }}
          </button>
        </div>
        <p v-if="bootMsg" class="text-xs -mt-2" :class="bootErr ? 'text-red-400' : 'text-green-400'">{{ bootMsg }}</p>

        <div class="flex items-center justify-between">
          <span class="text-sm text-text-dim">Reboot Device:</span>
          <button @click="reboot" class="bg-accent hover:bg-accent-hover text-white px-4 py-1.5 rounded text-sm transition-colors">
            Reboot
          </button>
        </div>

        <div class="flex items-center justify-between">
          <span class="text-sm text-text-dim">Reset Firmware:</span>
          <button @click="confirmReset" class="bg-red-600 hover:bg-red-700 text-white px-4 py-1.5 rounded text-sm transition-colors">
            Reset
          </button>
        </div>
        <p v-if="resetConfirm" class="text-xs text-red-400">
          This will erase all settings (WiFi, audio, password) and reboot.
          <button @click="resetDefaults" class="underline font-semibold ml-1">Confirm</button>
          <button @click="resetConfirm = false" class="underline ml-2">Cancel</button>
        </p>
      </div>

      <p v-if="sysMsg" class="text-xs mt-2" :class="sysErr ? 'text-red-400' : 'text-green-400'">{{ sysMsg }}</p>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">OTA Update</h2>
      <p class="text-xs text-text-dim mb-3">Upload a firmware (.bin) or SPIFFS image — file type is auto-detected.</p>
      <input type="file" ref="fileInput" accept=".bin" class="hidden" @change="onFileChange">
      <div class="flex items-center gap-2">
        <button @click="fileInput?.click()" :disabled="uploading" class="shrink-0 bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors disabled:opacity-50">
          Choose File
        </button>
        <span class="text-sm text-text-dim truncate min-w-0 flex-1">{{ fileName || 'No file selected' }}</span>
        <button @click="upload" :disabled="uploading" class="shrink-0 bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors disabled:opacity-50">
          {{ uploading ? 'Uploading...' : 'Upload' }}
        </button>
      </div>

      <div v-if="progress >= 0" class="mt-3">
        <div class="w-full bg-input rounded-full h-2">
          <div class="bg-accent h-2 rounded-full transition-all" :style="{ width: progress + '%' }"></div>
        </div>
        <p class="text-xs text-text-dim mt-1">{{ progress }}%</p>
      </div>
      <p v-if="uploadMsg" class="text-xs mt-2" :class="uploadErr ? 'text-red-400' : 'text-green-400'">{{ uploadMsg }}</p>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { apiGet, api } from '../../api.js'
import { useRebootWatchdog } from '../../composables/useRebootWatchdog.js'

const rebootWatchdog = useRebootWatchdog()

const info = ref({})
const apSsid = ref('Chute-Setup')
const fileInput = ref(null)
const fileName = ref('')
const uploading = ref(false)
const progress = ref(-1)
const uploadMsg = ref('')
const uploadErr = ref(false)
const bootMsg = ref('')
const bootErr = ref(false)
const sysMsg = ref('')
const sysErr = ref(false)
const resetConfirm = ref(false)

onMounted(async () => {
  try {
    info.value = await apiGet('/api/info')
    apSsid.value = info.value.ap_ssid || 'Chute-Setup'
  } catch (e) {
    console.error(e)
  }
})

async function switchBoot() {
  bootMsg.value = ''
  try {
    const res = await api('/api/firmware/boot', { method: 'POST' })
    const data = await res.json()
    if (res.ok) {
      info.value = await apiGet('/api/info')
      bootMsg.value = 'Boot partition switched (reboot to activate)'
      bootErr.value = false
    } else {
      bootMsg.value = data.message || 'Failed'
      bootErr.value = true
    }
  } catch (e) {
    bootMsg.value = 'Failed'
    bootErr.value = true
  }
}

async function reboot() {
  sysMsg.value = ''
  try {
    await api('/api/system/reboot', { method: 'POST' })
    rebootWatchdog.start(apSsid.value)
  } catch (e) {
    sysMsg.value = 'Reboot failed'
    sysErr.value = true
  }
}

function confirmReset() {
  resetConfirm.value = true
}

async function resetDefaults() {
  resetConfirm.value = false
  sysMsg.value = ''
  try {
    await api('/api/system/reset', { method: 'POST' })
    rebootWatchdog.start('Chute-Setup')
  } catch (e) {
    sysMsg.value = 'Reset failed'
    sysErr.value = true
  }
}

function onFileChange() {
  fileName.value = fileInput.value?.files?.[0]?.name || ''
}

function upload() {
  const file = fileInput.value?.files?.[0]
  if (!file) { uploadMsg.value = 'Select a .bin file first'; uploadErr.value = true; return }

  uploading.value = true
  progress.value = 0
  uploadMsg.value = ''
  uploadErr.value = false

  const xhr = new XMLHttpRequest()
  xhr.open('POST', '/api/firmware/upload')

  const auth = sessionStorage.getItem('chute_auth')
  if (auth) xhr.setRequestHeader('Authorization', 'Basic ' + auth)

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) progress.value = Math.round((e.loaded / e.total) * 100)
  }
  xhr.onload = () => {
    uploading.value = false
    if (xhr.status === 200) {
      try {
        const data = JSON.parse(xhr.responseText)
        if (data.type === 'spiffs') {
          uploadMsg.value = 'Web UI updated!'
        } else {
          uploadMsg.value = ''
          rebootWatchdog.start(apSsid.value)
        }
      } catch {
        uploadMsg.value = 'Upload complete'
      }
      uploadErr.value = false
    } else {
      uploadMsg.value = 'Error: ' + xhr.responseText
      uploadErr.value = true
    }
  }
  xhr.onerror = () => {
    uploading.value = false
    uploadMsg.value = 'Upload failed'
    uploadErr.value = true
  }
  xhr.send(file)
}
</script>
