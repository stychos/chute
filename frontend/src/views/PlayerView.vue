<template>
  <div class="min-h-screen bg-bg flex flex-col items-center justify-center p-4 relative">
    <router-link v-if="!configOpen" to="/config" class="absolute top-4 right-4 text-text-dim hover:text-accent transition-colors" title="Settings">
      <svg xmlns="http://www.w3.org/2000/svg" class="w-6 h-6" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
        <path stroke-linecap="round" stroke-linejoin="round" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" />
        <path stroke-linecap="round" stroke-linejoin="round" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
      </svg>
    </router-link>

    <div class="w-full flex flex-col items-center">
      <!-- Hardware warning -->
      <div v-if="stream.hwWarning.value && !hwDismissed" class="mb-4 w-full max-w-2xl bg-yellow-900/30 border border-yellow-600/50 rounded-lg px-4 py-3 flex items-start gap-3">
        <span class="text-yellow-400 text-sm flex-1">{{ stream.hwWarning.value }}</span>
        <button @click="hwDismissed = true" class="text-yellow-400/60 hover:text-yellow-400 shrink-0" title="Dismiss">
          <svg xmlns="http://www.w3.org/2000/svg" class="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
            <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" />
          </svg>
        </button>
      </div>

      <!-- Video + audio player -->
      <div v-if="stream.hasCamera.value" class="group bg-black rounded-lg overflow-hidden relative w-full cursor-pointer"
        :style="{ aspectRatio: stream.camWidth.value + '/' + stream.camHeight.value, maxWidth: stream.camWidth.value + 'px' }"
        @click="stream.togglePlay()">
        <img v-show="stream.playing.value" ref="vidEl" class="w-full h-full object-contain" alt="Video">
        <!-- Play overlay when stopped -->
        <div v-if="!stream.playing.value" class="absolute inset-0 flex items-center justify-center">
          <svg xmlns="http://www.w3.org/2000/svg" class="w-16 h-16 text-white/70" viewBox="0 0 24 24" fill="currentColor">
            <path d="M8 5v14l11-7z" />
          </svg>
        </div>
        <!-- Pause overlay on hover when playing -->
        <div v-if="stream.playing.value" class="absolute inset-0 flex items-center justify-center bg-black/30 opacity-0 group-hover:opacity-100 transition-opacity">
          <svg xmlns="http://www.w3.org/2000/svg" class="w-16 h-16 text-white/70" viewBox="0 0 24 24" fill="currentColor">
            <path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z" />
          </svg>
        </div>
        <input v-if="stream.hasMic.value && stream.playing.value" type="range"
          min="0" max="1" step="0.05" :value="stream.volume.value"
          @input="stream.setVolume(+$event.target.value)"
          @click.stop
          class="absolute bottom-3 left-[5%] w-[90%] accent-accent cursor-pointer opacity-0 group-hover:opacity-100 transition-opacity z-10"
          title="Volume">
      </div>

      <!-- Audio-only view when no camera -->
      <div v-else-if="stream.hasMic.value" class="bg-card rounded-lg p-8 flex flex-col items-center gap-4">
        <p v-if="!stream.playing.value" class="text-text-dim">Audio Only</p>
        <input v-if="stream.playing.value" type="range"
          min="0" max="1" step="0.05" :value="stream.volume.value"
          @input="stream.setVolume(+$event.target.value)"
          class="w-full max-w-md accent-accent cursor-pointer"
          title="Volume">
      </div>

      <!-- No hardware -->
      <div v-else class="bg-card rounded-lg p-8 text-center text-text-dim">
        No camera or microphone detected.
      </div>

      <div v-if="stream.hasCamera.value || stream.hasMic.value" class="flex justify-center mt-4">
        <button @click="stream.togglePlay()" class="bg-accent hover:bg-accent-hover text-white px-8 py-2.5 rounded font-medium transition-colors">
          {{ stream.playing.value ? 'Stop' : 'Play' }}
        </button>
      </div>
    </div>

    <!-- Config panel renders here as overlay -->
    <router-view />
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useRoute } from 'vue-router'
import { useStreamController } from '../composables/useStreamController.js'

const route = useRoute()
const stream = useStreamController()
const vidEl = ref(null)
const hwDismissed = ref(false)

const configOpen = computed(() => route.path.startsWith('/config'))

onMounted(async () => {
  await stream.init()
  stream.registerElements(vidEl)
})

onUnmounted(() => {
  stream.unregisterElements()
})
</script>
