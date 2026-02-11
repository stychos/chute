<template>
  <div class="space-y-3">
    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">WiFi Configuration</h2>

      <label class="block text-sm text-text-dim mb-1">WiFi Mode</label>
      <select v-model="wifiMode" class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">
        <option value="auto">Auto (STA with AP fallback)</option>
        <option value="sta">STA (Connect to WiFi network)</option>
        <option value="ap">AP (Create a WiFi network)</option>
      </select>

      <template v-if="wifiMode !== 'ap'">
        <label class="block text-sm text-text-dim mb-1">SSID</label>
        <input v-model="ssid" type="text" maxlength="63"
          class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">

        <label class="block text-sm text-text-dim mb-1">Password</label>
        <input v-model="password" :type="showPw ? 'text' : 'password'" maxlength="63" placeholder="WiFi password"
          class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-1">
        <label class="flex items-center gap-1.5 text-xs text-text-dim mb-3 cursor-pointer">
          <input type="checkbox" v-model="showPw" class="accent-accent"> Show password
        </label>
        <p v-if="!password && passwordSet" class="text-xs text-text-dim -mt-2 mb-3">A password is currently set. Re-enter it to keep it.</p>
      </template>

      <template v-else>
        <label class="block text-sm text-text-dim mb-1">AP Network Name</label>
        <input v-model="apSsid" type="text" maxlength="31" placeholder="Chute-Setup"
          class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">

        <label class="block text-sm text-text-dim mb-1">AP Password</label>
        <input v-model="apPassword" :type="showApPw ? 'text' : 'password'" maxlength="63" placeholder="Leave empty for open network"
          class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-1">
        <label class="flex items-center gap-1.5 text-xs text-text-dim mb-3 cursor-pointer">
          <input type="checkbox" v-model="showApPw" class="accent-accent"> Show password
        </label>
        <p v-if="apPassword && apPassword.length < 8" class="text-xs text-yellow-400 -mt-2 mb-3">Password must be at least 8 characters</p>
        <p v-else-if="!apPassword && apPasswordSet" class="text-xs text-text-dim -mt-2 mb-3">A password is currently set. Re-enter it or leave empty for open network.</p>
      </template>

      <label class="block text-sm text-text-dim mb-1">Hostname</label>
      <input v-model="hostname" type="text" maxlength="31" placeholder="chute"
        class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">

      <div v-if="networks.length" class="mb-3 max-h-40 overflow-y-auto border border-border rounded">
        <button v-for="net in networks" :key="net.ssid + net.rssi"
          @click="selectNetwork(net.ssid)"
          class="w-full text-left px-3 py-1.5 text-sm hover:bg-input transition-colors flex justify-between"
          :class="wifiMode !== 'ap' && ssid === net.ssid ? 'text-accent' : 'text-text'">
          <span>{{ net.ssid }}</span>
          <span class="text-text-dim text-xs">{{ net.rssi }} dBm · {{ net.auth }}</span>
        </button>
      </div>

      <div class="flex justify-between items-start">
        <div>
          <button @click="save" class="bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors">
            Save &amp; Reboot
          </button>
          <p v-if="msg" class="text-xs mt-2" :class="msgErr ? 'text-red-400' : 'text-green-400'">{{ msg }}</p>
        </div>
        <button @click="scan" :disabled="scanning"
          class="bg-card border border-border hover:border-accent text-text-dim hover:text-accent px-3 py-2 rounded text-sm transition-colors">
          {{ scanning ? 'Scanning...' : 'Scan' }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { apiGet, apiPost } from '../../api.js'
import { useRebootWatchdog } from '../../composables/useRebootWatchdog.js'

const rebootWatchdog = useRebootWatchdog()

const ssid = ref('')
const password = ref('')
const wifiMode = ref('auto')
const apSsid = ref('Chute-Setup')
const apPassword = ref('')
const passwordSet = ref(false)
const apPasswordSet = ref(false)
const hostname = ref('chute')
const showPw = ref(false)
const showApPw = ref(false)
const scanning = ref(false)
const networks = ref([])
const msg = ref('')
const msgErr = ref(false)

onMounted(async () => {
  try {
    const info = await apiGet('/api/info')
    ssid.value = info.ssid || ''
    password.value = info.password || ''
    wifiMode.value = info.wifi_mode_pref || 'auto'
    apSsid.value = info.ap_ssid || 'Chute-Setup'
    apPassword.value = info.ap_password || ''
    if (info.password_set) passwordSet.value = true
    if (info.ap_password_set) apPasswordSet.value = true
    hostname.value = info.hostname || 'chute'
  } catch (e) {
    console.error(e)
  }
})

async function scan() {
  scanning.value = true
  networks.value = []
  try {
    const res = await apiGet('/api/wifi/scan')
    networks.value = res.networks || []
  } catch (e) {
    console.error('Scan failed', e)
  }
  scanning.value = false
}

function selectNetwork(name) {
  if (wifiMode.value !== 'ap') {
    ssid.value = name
  }
}

async function save() {
  msg.value = ''
  if (!ssid.value && wifiMode.value !== 'ap') {
    msg.value = 'SSID is required'
    msgErr.value = true
    return
  }
  if (wifiMode.value === 'ap' && apPassword.value && apPassword.value.length < 8) {
    msg.value = 'AP password must be at least 8 characters'
    msgErr.value = true
    return
  }
  try {
    const body = {
      ssid: ssid.value,
      password: password.value,
      wifi_mode: wifiMode.value,
      hostname: hostname.value
    }
    if (wifiMode.value === 'ap') {
      if (apSsid.value) body.ap_ssid = apSsid.value
      body.ap_password = apPassword.value
    }
    const res = await apiPost('/api/wifi/config', body)
    if (res.ok) {
      msg.value = ''
      rebootWatchdog.start(apSsid.value || 'Chute-Setup')
    } else {
      msg.value = 'Save failed'
      msgErr.value = true
    }
  } catch (e) {
    msg.value = 'Save failed'
    msgErr.value = true
  }
}
</script>
