<script setup lang="ts">
import { ref, type CSSProperties } from 'vue';

interface PerformanceStats {
  fps: number;
  bitrateMbps: number;
  decodeQueueSize: number;
  lossEvents: number;
}

defineProps<{
  frameVisible: boolean;
  controlActive: boolean;
  status: string;
  statusError: boolean;
  canvasStyle: CSSProperties;
  cursorStyle: CSSProperties;
  performanceStats: PerformanceStats;
}>();

defineEmits<{
  capture: [];
  stop: [];
}>();

const canvas = ref<HTMLCanvasElement | null>(null);
const showPerformance = ref(false);

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
    <div class="remote-toolbar" :class="{ 'show-performance': showPerformance }">
      <div class="remote-toolbar-main">
        <span id="remote-status" :class="{ error: statusError }">{{ status }}</span>
        <label class="performance-toggle">
          <input v-model="showPerformance" type="checkbox">
          <span>性能</span>
        </label>
        <button id="remote-stop" type="button" @click="$emit('stop')">断开</button>
      </div>
      <dl v-if="showPerformance" class="performance-stats">
        <div><dt>解码 FPS</dt><dd>{{ performanceStats.fps.toFixed(1) }}</dd></div>
        <div><dt>接收码率</dt><dd>{{ performanceStats.bitrateMbps.toFixed(1) }} Mbps</dd></div>
        <div><dt>解码队列</dt><dd>{{ performanceStats.decodeQueueSize }}</dd></div>
        <div><dt>丢帧事件</dt><dd>{{ performanceStats.lossEvents }}</dd></div>
      </dl>
    </div>
  </section>
</template>
