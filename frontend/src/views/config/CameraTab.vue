<template>
  <div class="space-y-3">
    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Camera — {{ sensor.name }}</h2>

      <div class="space-y-3">
        <div>
          <label class="block text-sm text-text-dim mb-1">Resolution</label>
          <select v-model.number="status.framesize" @change="setVar('framesize', status.framesize)"
            class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm">
            <option v-for="r in sensor.resolutions" :key="r.value" :value="r.value">{{ r.label }}</option>
          </select>
        </div>

        <Slider label="Quality" var-name="quality" v-model="status.quality" :min="4" :max="63" @update="setVar" />
        <Slider label="Brightness" var-name="brightness" v-model="status.brightness" :min="-2" :max="2" @update="setVar" />
        <Slider label="Contrast" var-name="contrast" v-model="status.contrast" :min="-2" :max="2" @update="setVar" />
        <Slider label="Saturation" var-name="saturation" v-model="status.saturation" :min="-2" :max="2" @update="setVar" />
        <Slider v-if="sensor.hasSharpness" label="Sharpness" var-name="sharpness" v-model="status.sharpness" :min="-2" :max="2" @update="setVar" />

        <div>
          <label class="block text-sm text-text-dim mb-1">Special Effect</label>
          <select v-model.number="status.special_effect" @change="setVar('special_effect', status.special_effect)"
            class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm">
            <option v-for="e in EFFECTS" :key="e.value" :value="e.value">{{ e.label }}</option>
          </select>
        </div>

        <div>
          <label class="block text-sm text-text-dim mb-1">White Balance Mode</label>
          <select v-model.number="status.wb_mode" @change="setVar('wb_mode', status.wb_mode)"
            class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm">
            <option v-for="w in WB_MODES" :key="w.value" :value="w.value">{{ w.label }}</option>
          </select>
        </div>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Auto Controls</h2>
      <div class="space-y-3">
        <Toggle label="AWB" var-name="awb" v-model="status.awb" @update="setVar" />
        <Toggle label="AWB Gain" var-name="awb_gain" v-model="status.awb_gain" @update="setVar" />
        <Toggle label="AEC (Auto Exposure)" var-name="aec" v-model="status.aec" @update="setVar" />
        <Toggle label="AEC DSP" var-name="aec2" v-model="status.aec2" @update="setVar" />
        <Slider label="AE Level" var-name="ae_level" v-model="status.ae_level" :min="-2" :max="2" @update="setVar" />
        <Slider label="AEC Value" var-name="aec_value" v-model="status.aec_value" :min="0" :max="1200" @update="setVar" />
        <Toggle label="AGC (Auto Gain)" var-name="agc" v-model="status.agc" @update="setVar" />
        <Slider label="AGC Gain" var-name="agc_gain" v-model="status.agc_gain" :min="0" :max="sensor.maxAgcGain" @update="setVar" />

        <div>
          <label class="block text-sm text-text-dim mb-1">Gain Ceiling</label>
          <select v-model.number="status.gainceiling" @change="setVar('gainceiling', status.gainceiling)"
            class="w-full px-3 py-2 bg-input border border-border rounded text-text text-sm">
            <option v-for="i in 7" :key="i-1" :value="i-1">{{ Math.pow(2, i) }}x</option>
          </select>
        </div>
      </div>
    </div>

    <div class="bg-card rounded-lg p-4">
      <h2 class="text-accent text-sm font-semibold mb-3">Corrections</h2>
      <div class="space-y-3">
        <Toggle label="BPC (Black Pixel)" var-name="bpc" v-model="status.bpc" @update="setVar" />
        <Toggle label="WPC (White Pixel)" var-name="wpc" v-model="status.wpc" @update="setVar" />
        <Toggle label="Raw GMA" var-name="raw_gma" v-model="status.raw_gma" @update="setVar" />
        <Toggle label="Lens Correction" var-name="lenc" v-model="status.lenc" @update="setVar" />
        <Toggle label="H-Mirror" var-name="hmirror" v-model="status.hmirror" @update="setVar" />
        <Toggle label="V-Flip" var-name="vflip" v-model="status.vflip" @update="setVar" />
        <Toggle label="DCW (Downsize)" var-name="dcw" v-model="status.dcw" @update="setVar" />
        <Toggle label="Color Bar" var-name="colorbar" v-model="status.colorbar" @update="setVar" />
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted } from 'vue'
import { apiGet, api } from '../../api.js'
import { getSensorConfig, EFFECTS, WB_MODES } from '../../components/camera/SensorConfig.js'
import Slider from '../../components/camera/Slider.vue'
import Toggle from '../../components/camera/Toggle.vue'
import { useStreamController } from '../../composables/useStreamController.js'

const stream = useStreamController()

const sensor = ref({ name: '...', resolutions: [], hasSharpness: false, maxAgcGain: 30 })
const status = reactive({})

onMounted(async () => {
  try {
    const [info, st] = await Promise.all([
      apiGet('/api/camera/info'),
      apiGet('/api/camera/status')
    ])
    sensor.value = getSensorConfig(info.pid)
    Object.assign(status, st)
  } catch (e) {
    console.error(e)
  }
})

function setVar(name, val) {
  api('/api/camera/control?var=' + name + '&val=' + val, { method: 'POST' })
  if (name === 'framesize') {
    stream.updateFrameDims(val)
    stream.restartVideo()
  }
}
</script>
