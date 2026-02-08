<template>
  <div>
    <label class="flex justify-between text-sm text-text-dim mb-1">
      <span>{{ label }}</span>
      <span class="text-text">{{ modelValue }}</span>
    </label>
    <input type="range" :min="min" :max="max" :value="modelValue"
      @input="onChange" class="w-full accent-accent">
  </div>
</template>

<script setup>
const props = defineProps({
  label: String,
  varName: String,
  modelValue: Number,
  min: { type: Number, default: 0 },
  max: { type: Number, default: 100 },
})

const emit = defineEmits(['update:modelValue', 'update'])

function onChange(e) {
  const val = Number(e.target.value)
  emit('update:modelValue', val)
  emit('update', props.varName, val)
}
</script>
