<template>
  <div class="flex items-center justify-between">
    <span class="text-sm text-text-dim">{{ label }}</span>
    <button @click="toggle"
      class="w-10 h-5 rounded-full transition-colors relative"
      :class="modelValue ? 'bg-accent' : 'bg-input border border-border'">
      <span class="absolute top-0.5 w-4 h-4 rounded-full bg-white transition-transform"
        :class="modelValue ? 'left-5' : 'left-0.5'"></span>
    </button>
  </div>
</template>

<script setup>
const props = defineProps({
  label: String,
  varName: String,
  modelValue: Number,
})

const emit = defineEmits(['update:modelValue', 'update'])

function toggle() {
  const val = props.modelValue ? 0 : 1
  emit('update:modelValue', val)
  emit('update', props.varName, val)
}
</script>
