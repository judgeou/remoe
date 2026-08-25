<script setup lang="ts">
import { ref, type CSSProperties } from 'vue';

defineProps<{
  frameVisible: boolean;
  controlActive: boolean;
  status: string;
  statusError: boolean;
  canvasStyle: CSSProperties;
  cursorStyle: CSSProperties;
}>();

defineEmits<{
  capture: [];
  stop: [];
}>();

const canvas = ref<HTMLCanvasElement | null>(null);

defineExpose({
  getCanvas: () => {
    if (!canvas.value) throw new Error('视频 Canvas 尚未挂载');
    return canvas.value;
  },
});
</script>

<template>
  <section v-show="frameVisible" class="viewer" aria-live="polite">
    <canvas id="video" ref="canvas" :style="canvasStyle"></canvas>
    <div id="remote-cursor" class="remote-cursor" :style="cursorStyle" aria-hidden="true"></div>
    <div v-if="frameVisible && !controlActive" id="control-gate" class="control-gate">
      <button id="capture-input" type="button" @click="$emit('capture')">点击画面接管键鼠</button>
      <span>按 Esc 随时释放</span>
    </div>
    <div class="remote-toolbar">
      <span id="remote-status" :class="{ error: statusError }">{{ status }}</span>
      <button id="remote-stop" type="button" @click="$emit('stop')">断开</button>
    </div>
  </section>
</template>
