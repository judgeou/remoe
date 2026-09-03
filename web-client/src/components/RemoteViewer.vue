<script setup lang="ts">
import { nextTick, ref, type CSSProperties } from 'vue';

interface PerformanceStats {
  fps: number;
  bitrateMbps: number;
  dataRateKBps: number;
  lostPackets: number;
  droppedFrames: number;
  endToEndMs: number;
  captureToReceiveMs: number;
  receiveToPresentMs: number;
  jitterBufferMs: number;
  jitterMinimumMs: number;
  jitterTargetMs: number;
  decodeMs: number;
  processingMs: number;
  decoderImplementation: string;
  powerEfficientDecoder: boolean | null;
}

defineProps<{
  frameVisible: boolean;
  controlActive: boolean;
  status: string;
  statusError: boolean;
  videoStyle: CSSProperties;
  cursorStyle: CSSProperties;
  performanceStats: PerformanceStats;
  touchPreferred: boolean;
  touchMode: 'trackpad' | 'direct';
  activeModifiers: string[];
  fullscreenActive: boolean;
  orientationLocked: boolean;
  wakeLockEnabled: boolean;
  remoteClipboardPending: boolean;
  viewportZoom: number;
}>();

const emit = defineEmits<{
  capture: [];
  stop: [];
  touchMode: [mode: 'trackpad' | 'direct'];
  virtualKey: [code: string];
  virtualModifier: [code: string];
  virtualMouse: [button: 'left' | 'right'];
  textInput: [text: string];
  sendClipboard: [];
  receiveClipboard: [];
  fullscreen: [];
  orientation: [];
  wakeLock: [];
  resetViewport: [];
}>();

const video = ref<HTMLVideoElement | null>(null);
const viewerElement = ref<HTMLElement | null>(null);
const mobileInput = ref<HTMLInputElement | null>(null);
const showPerformance = ref(false);
const showMobileKeyboard = ref(false);
const showMobilePanel = ref(false);
const mobileToolbarHidden = ref(false);

const modifierKeys = [
  { code: 'ControlLeft', label: 'Ctrl' },
  { code: 'AltLeft', label: 'Alt' },
  { code: 'ShiftLeft', label: 'Shift' },
  { code: 'MetaLeft', label: 'Win' },
];

const functionKeys = [
  { code: 'Escape', label: 'Esc' },
  { code: 'Tab', label: 'Tab' },
  { code: 'Backspace', label: '⌫' },
  { code: 'Enter', label: 'Enter' },
  { code: 'ArrowLeft', label: '←' },
  { code: 'ArrowUp', label: '↑' },
  { code: 'ArrowDown', label: '↓' },
  { code: 'ArrowRight', label: '→' },
];

function toggleMobileKeyboard() {
  showMobileKeyboard.value = !showMobileKeyboard.value;
  if (showMobileKeyboard.value) {
    showMobilePanel.value = false;
    void nextTick(() => mobileInput.value?.focus());
  }
}

function toggleMobilePanel() {
  showMobilePanel.value = !showMobilePanel.value;
  if (showMobilePanel.value) {
    showMobileKeyboard.value = false;
    mobileInput.value?.blur();
  }
}

function hideMobileToolbar() {
  showMobileKeyboard.value = false;
  showMobilePanel.value = false;
  mobileInput.value?.blur();
  mobileToolbarHidden.value = true;
}

function handleBeforeInput(event: InputEvent) {
  event.preventDefault();
  if (event.inputType.startsWith('delete')) {
    emit('virtualKey', 'Backspace');
  } else if (event.data) {
    emit('textInput', event.data);
  }
}

function handleMobileKeyDown(event: KeyboardEvent) {
  event.stopPropagation();
  if (event.key === 'Enter' || event.key === 'Backspace') {
    event.preventDefault();
    emit('virtualKey', event.key);
  }
}

function clearMobileInput(event: Event) {
  (event.target as HTMLInputElement).value = '';
}

defineExpose({
  getVideo: () => {
    if (!video.value) throw new Error('视频元素尚未挂载');
    return video.value;
  },
  getElement: () => {
    if (!viewerElement.value) throw new Error('远程画面容器尚未挂载');
    return viewerElement.value;
  },
});
</script>

<template>
  <section v-show="frameVisible" ref="viewerElement" class="viewer" aria-live="polite">
    <video
      id="video"
      ref="video"
      class="viewer-video"
      :style="videoStyle"
      autoplay
      muted
      playsinline
      disablepictureinpicture
      @click="!controlActive && emit('capture')"
    ></video>
    <div id="remote-cursor" class="remote-cursor" :style="cursorStyle" aria-hidden="true"></div>
    <div
      class="remote-toolbar"
      :class="{ 'show-performance': showPerformance, 'mobile-toolbar-hidden': mobileToolbarHidden }"
    >
      <button
        v-if="mobileToolbarHidden"
        type="button"
        class="mobile-toolbar-handle"
        aria-label="展开远程控制工具栏"
        @click="mobileToolbarHidden = false"
      >控制</button>
      <div v-show="!mobileToolbarHidden" class="remote-toolbar-main">
        <span id="remote-status" :class="{ error: statusError }">{{ status }}</span>
        <button type="button" title="把浏览器剪贴板发送到远程电脑" @click="emit('sendClipboard')">
          发送剪贴板
        </button>
        <button
          type="button"
          title="把远程电脑剪贴板写入浏览器"
          :class="{ active: remoteClipboardPending }"
          :disabled="!remoteClipboardPending"
          @click="emit('receiveClipboard')"
        >接收剪贴板</button>
        <label class="performance-toggle">
          <input v-model="showPerformance" type="checkbox">
          <span>性能</span>
        </label>
        <button id="remote-stop" type="button" @click="emit('stop')">断开</button>
      </div>
      <dl v-if="showPerformance && !mobileToolbarHidden" class="performance-stats">
        <div><dt>解码 FPS</dt><dd>{{ performanceStats.fps.toFixed(1) }}</dd></div>
        <div><dt>接收码率</dt><dd>{{ performanceStats.bitrateMbps.toFixed(1) }} Mbps</dd></div>
        <div><dt>实际网速</dt><dd>{{ performanceStats.dataRateKBps.toFixed(1) }} KB/s</dd></div>
        <div><dt>端到端</dt><dd>{{ performanceStats.endToEndMs.toFixed(1) }} ms</dd></div>
        <div><dt>捕获到接收</dt><dd>{{ performanceStats.captureToReceiveMs.toFixed(1) }} ms</dd></div>
        <div><dt>接收到呈现</dt><dd>{{ performanceStats.receiveToPresentMs.toFixed(1) }} ms</dd></div>
        <div><dt>抖动缓冲</dt><dd>{{ performanceStats.jitterBufferMs.toFixed(1) }} ms</dd></div>
        <div><dt>缓冲目标</dt><dd>{{ performanceStats.jitterTargetMs.toFixed(1) }} ms</dd></div>
        <div><dt>缓冲下限</dt><dd>{{ performanceStats.jitterMinimumMs.toFixed(1) }} ms</dd></div>
        <div><dt>解码耗时</dt><dd>{{ performanceStats.decodeMs.toFixed(1) }} ms</dd></div>
        <div><dt>接收到解码</dt><dd>{{ performanceStats.processingMs.toFixed(1) }} ms</dd></div>
        <div><dt>本周期丢包</dt><dd>{{ performanceStats.lostPackets }}</dd></div>
        <div><dt>本周期掉帧</dt><dd>{{ performanceStats.droppedFrames }}</dd></div>
        <div><dt>节能解码</dt><dd>{{ performanceStats.powerEfficientDecoder === null
          ? '未知' : (performanceStats.powerEfficientDecoder ? '是' : '否') }}</dd></div>
        <div class="performance-decoder"><dt>解码器</dt><dd>{{ performanceStats.decoderImplementation || '未知' }}</dd></div>
      </dl>
      <div v-show="!mobileToolbarHidden" class="mobile-controls" :class="{ expanded: showMobilePanel || showMobileKeyboard }">
        <div class="mobile-quick-actions">
          <div class="touch-mode-switch" role="group" aria-label="触控方式">
            <button
              type="button"
              :class="{ active: touchMode === 'trackpad' }"
              @click="emit('touchMode', 'trackpad')"
            >触控板</button>
            <button
              type="button"
              :class="{ active: touchMode === 'direct' }"
              @click="emit('touchMode', 'direct')"
            >直触</button>
          </div>
          <button
            type="button"
            :class="{ active: showMobileKeyboard }"
            @click="toggleMobileKeyboard"
          >键盘</button>
          <button type="button" :class="{ active: showMobilePanel }" @click="toggleMobilePanel">更多</button>
          <button type="button" @click="hideMobileToolbar">隐藏</button>
          <button type="button" class="mobile-stop" @click="emit('stop')">断开</button>
        </div>
        <div v-show="showMobilePanel" class="mobile-panel">
          <p class="touch-help">
            {{ touchMode === 'trackpad'
              ? '单指移动/点击 · 双击拖动 · 双指滚动/缩放；放大后拖动画面 · 双指轻点右键'
              : '点击定位 · 按住拖动；可用键盘面板中的右键按钮' }}
          </p>
          <div class="mobile-display-actions">
            <button type="button" @click="emit('fullscreen')">{{ fullscreenActive ? '退出全屏' : '全屏' }}</button>
            <button type="button" :class="{ active: orientationLocked }" @click="emit('orientation')">横屏</button>
            <button type="button" :class="{ active: wakeLockEnabled }" @click="emit('wakeLock')">常亮</button>
            <button type="button" :class="{ active: showPerformance }" @click="showPerformance = !showPerformance">性能</button>
            <button type="button" :disabled="viewportZoom <= 1" @click="emit('resetViewport')">
              画面 {{ Math.round(viewportZoom * 100) }}%
            </button>
            <!-- <button type="button" @click="emit('sendClipboard')">发送剪贴板</button>
            <button
              type="button"
              :class="{ active: remoteClipboardPending }"
              :disabled="!remoteClipboardPending"
              @click="emit('receiveClipboard')"
            >接收剪贴板</button> -->
          </div>
        </div>
        <div v-show="showMobileKeyboard" class="mobile-keyboard">
          <input
            ref="mobileInput"
            type="text"
            inputmode="text"
            enterkeyhint="enter"
            autocomplete="off"
            autocapitalize="off"
            autocorrect="off"
            spellcheck="false"
            placeholder="在此输入英文和常用符号"
            aria-label="发送文字到远程电脑"
            @beforeinput="handleBeforeInput"
            @input="clearMobileInput"
            @keydown="handleMobileKeyDown"
            @keyup.stop
          >
          <div class="virtual-key-grid modifiers">
            <button
              v-for="key in modifierKeys"
              :key="key.code"
              type="button"
              :class="{ active: activeModifiers.includes(key.code) }"
              @click="emit('virtualModifier', key.code)"
            >{{ key.label }}</button>
          </div>
          <div class="virtual-key-grid">
            <button
              v-for="key in functionKeys"
              :key="key.code"
              type="button"
              @click="emit('virtualKey', key.code)"
            >{{ key.label }}</button>
            <button type="button" @click="emit('virtualMouse', 'left')">左键</button>
            <button type="button" @click="emit('virtualMouse', 'right')">右键</button>
          </div>
        </div>
      </div>
    </div>
  </section>
</template>
