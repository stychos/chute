<template>
  <div class="bg-card rounded-lg p-4">
    <h2 class="text-accent text-sm font-semibold mb-3">Settings Password</h2>
    <p class="text-xs text-text-dim mb-3">Protects config pages. Leave empty to disable authentication.</p>

    <label class="block text-sm text-text-dim mb-1">New Password</label>
    <input v-model="pw" :type="show ? 'text' : 'password'" maxlength="63" placeholder="Leave empty to disable"
      class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-1">
    <label class="flex items-center gap-1.5 text-xs text-text-dim mb-3 cursor-pointer">
      <input type="checkbox" v-model="show" class="accent-accent"> Show password
    </label>

    <button @click="save" class="bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors">
      Save
    </button>
    <p v-if="msg" class="text-xs mt-2" :class="msgErr ? 'text-red-400' : 'text-green-400'">{{ msg }}</p>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { apiPost, setAuth } from '../../api.js'

const pw = ref('')
const show = ref(false)
const msg = ref('')
const msgErr = ref(false)

async function save() {
  try {
    const res = await apiPost('/api/auth/password', { password: pw.value })
    if (res.ok) {
      setAuth(pw.value)
      msg.value = pw.value ? 'Password saved' : 'Authentication disabled'
      msgErr.value = false
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
