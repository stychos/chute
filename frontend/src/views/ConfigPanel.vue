<template>
  <Transition name="panel">
    <div class="fixed inset-0 z-40" @click.self="close">
      <!-- Backdrop -->
      <div class="absolute inset-0 bg-black/50 sm:bg-black/30" @click="close"></div>

      <!-- Panel -->
      <div class="absolute right-0 top-0 h-full w-full sm:w-[28rem] bg-bg text-text border-l border-border overflow-y-auto" @click.stop>
        <div class="px-4 py-4">
          <div class="flex items-center justify-between mb-4">
            <span class="text-text-dim text-sm">{{ stream.hostname.value }}</span>
            <button @click="close" class="text-text-dim hover:text-accent transition-colors" title="Close">
              <svg xmlns="http://www.w3.org/2000/svg" class="w-5 h-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
                <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" />
              </svg>
            </button>
          </div>

          <div v-if="needsAuth" class="bg-card rounded-lg p-6">
            <h2 class="text-accent text-sm font-semibold mb-3">Authentication Required</h2>
            <input v-model="password" type="password" placeholder="Settings password" @keyup.enter="doAuth"
              class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm mb-3">
            <button @click="doAuth" class="bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors">
              Login
            </button>
            <p v-if="authError" class="text-red-400 text-xs mt-2">Invalid password</p>
          </div>

          <template v-else>
            <nav class="flex gap-1 overflow-x-auto mb-4 pb-1 border-b border-border">
              <router-link v-for="tab in visibleTabs" :key="tab.path" :to="'/config/' + tab.path"
                class="px-3 py-2 text-sm whitespace-nowrap rounded-t transition-colors"
                :class="isActive(tab.path) ? 'bg-card text-accent border-b-2 border-accent' : 'text-text-dim hover:text-text'">
                {{ tab.label }}
              </router-link>
            </nav>
            <router-view />
          </template>
        </div>
        <RebootModal />
      </div>
    </div>
  </Transition>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { apiGet, setAuth } from '../api.js'
import { useStreamController } from '../composables/useStreamController.js'
import RebootModal from '../components/RebootModal.vue'

const route = useRoute()
const router = useRouter()
const stream = useStreamController()

const needsAuth = ref(false)
const password = ref('')
const authError = ref(false)

const allTabs = [
  { path: 'status', label: 'Status', always: true },
  { path: 'wifi', label: 'WiFi', always: true },
  { path: 'audio', label: 'Audio', requires: 'mic' },
  { path: 'camera', label: 'Camera', requires: 'camera' },
  { path: 'flash', label: 'Flash', always: true },
  { path: 'password', label: 'Password', always: true },
  { path: 'firmware', label: 'Firmware', always: true },
]

const visibleTabs = computed(() => {
  return allTabs.filter(tab => {
    if (tab.always) return true
    if (tab.requires === 'camera') return stream.hasCamera.value
    if (tab.requires === 'mic') return stream.hasMic.value
    return true
  })
})

function isActive(path) {
  return route.path === '/config/' + path
}

function close() {
  router.push('/')
}

onMounted(async () => {
  try {
    const authRes = await apiGet('/api/auth/check')
    if (authRes.auth_enabled && !authRes.valid) {
      needsAuth.value = true
    }
  } catch (e) {
    if (e.message === 'auth') {
      needsAuth.value = true
    }
  }
})

async function doAuth() {
  authError.value = false
  setAuth(password.value)
  try {
    const res = await apiGet('/api/auth/check')
    if (res.valid) {
      needsAuth.value = false
    } else {
      authError.value = true
    }
  } catch {
    authError.value = true
  }
}
</script>

<style scoped>
.panel-enter-active,
.panel-leave-active {
  transition: opacity 0.2s ease;
}
.panel-enter-active > div:last-child,
.panel-leave-active > div:last-child {
  transition: transform 0.2s ease;
}
.panel-enter-from,
.panel-leave-to {
  opacity: 0;
}
.panel-enter-from > div:last-child,
.panel-leave-to > div:last-child {
  transform: translateX(100%);
}
</style>
