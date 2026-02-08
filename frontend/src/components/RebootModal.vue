<template>
  <Teleport to="body">
    <div v-if="active" class="fixed inset-0 z-50 flex items-center justify-center bg-black/70">
      <div class="bg-card rounded-lg p-6 max-w-sm w-full mx-4 text-center">
        <template v-if="status === 'waiting'">
          <div class="flex justify-center mb-3">
            <svg class="animate-spin w-8 h-8 text-accent" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24">
              <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
              <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
            </svg>
          </div>
          <p class="text-text text-sm font-medium">Waiting for board to reboot...</p>
          <p class="text-text-dim text-xs mt-1">Checking connection every few seconds</p>
        </template>

        <template v-else-if="status === 'online'">
          <div class="flex justify-center mb-3 text-green-400">
            <svg class="w-8 h-8" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
              <path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7" />
            </svg>
          </div>
          <p class="text-green-400 text-sm font-medium">Board is back online!</p>
        </template>

        <template v-else-if="status === 'timeout'">
          <div class="flex justify-center mb-3 text-yellow-400">
            <svg class="w-8 h-8" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
              <path stroke-linecap="round" stroke-linejoin="round" d="M12 9v2m0 4h.01M12 3a9 9 0 100 18 9 9 0 000-18z" />
            </svg>
          </div>
          <p class="text-yellow-400 text-sm font-medium">Board is offline for too long</p>
          <p class="text-text-dim text-xs mt-2">
            The board may have entered fallback AP mode. Try connecting to the WiFi network
            <span class="text-text font-medium">{{ apSsid }}</span> and open
            <a :href="'http://' + apIp" class="text-accent underline">http://{{ apIp }}</a>
          </p>
          <button @click="stop" class="mt-4 bg-accent hover:bg-accent-hover text-white px-4 py-2 rounded text-sm transition-colors">
            Dismiss
          </button>
        </template>
      </div>
    </div>
  </Teleport>
</template>

<script setup>
import { useRebootWatchdog } from '../composables/useRebootWatchdog.js'
const { active, status, apSsid, apIp, stop } = useRebootWatchdog()
</script>
