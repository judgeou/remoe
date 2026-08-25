<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, reactive, ref, shallowRef, type CSSProperties } from 'vue';
import ConnectPanel from './components/ConnectPanel.vue';
import RemoteViewer from './components/RemoteViewer.vue';
import { RemoteInputController } from './core/input.js';
import { cursorViewportPosition, fitVideoSize } from './core/layout.js';
import { RemoeBrowserClient, parseInvite } from './core/remoe-client.js';

interface StreamDescription {
  width: number;
  height: number;
  fpsNum: number;
  bitrateBps: number;
  codec: string;
}

interface CursorPosition {
  x: number;
  y: number;
}

const viewer = ref<InstanceType<typeof RemoteViewer> | null>(null);
const invite = ref('');
const fps = ref(60);
const bitrate = ref(20);
const scale = ref(100);
const running = ref(false);
const supported = ref(true);
const status = ref('尚未连接');
const statusError = ref(false);
const details = ref('');
const remoteActive = ref(false);
const controlActive = ref(false);
const frameVisible = ref(false);
const canvasStyle = reactive<CSSProperties>({});
const cursorStyle = reactive<CSSProperties>({});
const client = shallowRef<RemoeBrowserClient | null>(null);
let inputController: RemoteInputController | null = null;
let streamSize: { width: number; height: number } | null = null;
let cursorPosition: CursorPosition = { x: 32768, y: 32768 };

function canvas(): HTMLCanvasElement {
  if (!viewer.value) throw new Error('远程画面尚未挂载');
  return viewer.value.getCanvas();
}

function setStatus(message: string, isError = false) {
  status.value = message;
  statusError.value = isError;
}

function setRemoteActive(active: boolean) {
  remoteActive.value = active;
  document.body.classList.toggle('remote-active', active);
  if (!active) document.body.classList.remove('control-active');
}

function positionRemoteCursor(position: CursorPosition = cursorPosition) {
  cursorPosition = position;
  const point = cursorViewportPosition(position.x, position.y, canvas().getBoundingClientRect());
  cursorStyle.left = `${point.left}px`;
  cursorStyle.top = `${point.top}px`;
}

function fitRemoteVideo() {
  if (!streamSize || !remoteActive.value) return;
  const fitted = fitVideoSize(
    streamSize.width,
    streamSize.height,
    document.documentElement.clientWidth,
    document.documentElement.clientHeight,
  );
  canvasStyle.width = `${fitted.width}px`;
  canvasStyle.height = `${fitted.height}px`;
  void nextTick(() => positionRemoteCursor());
}

function leaveRemoteMode() {
  inputController?.dispose();
  inputController = null;
  controlActive.value = false;
  frameVisible.value = false;
  streamSize = null;
  delete canvasStyle.width;
  delete canvasStyle.height;
  setRemoteActive(false);
}

function stopSession() {
  leaveRemoteMode();
  client.value?.stop();
  client.value = null;
  running.value = false;
}

async function connect() {
  try {
    stopSession();
    details.value = '';
    const parsedInvite = parseInvite(invite.value);
    running.value = true;
    const nextClient = new RemoeBrowserClient(parsedInvite, {
      fps: fps.value,
      bitrateMbps: bitrate.value,
      scalePercent: scale.value,
    }, {
      onStatus: (message: string) => setStatus(message),
      onIceState: (state: string) => { details.value = `ICE: ${state}`; },
      onStream: (stream: StreamDescription) => {
        streamSize = { width: stream.width, height: stream.height };
        const target = canvas();
        target.width = stream.width;
        target.height = stream.height;
        setRemoteActive(true);
        fitRemoteVideo();
        details.value = `${stream.width}×${stream.height} · ${stream.fpsNum} fps · ` +
          `${(stream.bitrateBps / 1_000_000).toFixed(1)} Mbps · ${stream.codec}`;
      },
      onFrame: (frame: VideoFrame) => {
        const target = canvas();
        const context = target.getContext('2d', { alpha: false });
        if (!context) throw new Error('浏览器无法创建 Canvas 2D context');
        context.drawImage(frame, 0, 0, target.width, target.height);
        if (frameVisible.value) return;
        frameVisible.value = true;
        void nextTick(() => {
          fitRemoteVideo();
          inputController = new RemoteInputController(
            target,
            (input: { type: number; flags?: number; value1?: number; value2?: number }) =>
              client.value?.sendInput(input) ?? false,
            (active: boolean) => {
              controlActive.value = active;
              document.body.classList.toggle('control-active', active);
              setStatus(active
                ? '正在控制远程桌面 · 按 Esc 释放键鼠'
                : '画面已连接 · 点击画面接管键鼠');
            },
            (position: CursorPosition) => positionRemoteCursor(position),
          );
        });
        setStatus('画面已连接 · 点击画面接管键鼠');
      },
      onError: (error: Error) => {
        leaveRemoteMode();
        setStatus(error.message, true);
        running.value = false;
      },
    });
    client.value = nextClient;
    await nextClient.connect();
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), true);
    running.value = false;
  }
}

async function captureInput() {
  try {
    await inputController?.capture();
  } catch (error) {
    setStatus(`无法锁定鼠标：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

onMounted(() => {
  if (location.hash.length > 1) invite.value = location.href;
  if (!globalThis.VideoDecoder) {
    supported.value = false;
    setStatus('当前浏览器没有 WebCodecs VideoDecoder，请使用最新版 Chrome 或 Edge', true);
  }
  window.addEventListener('resize', fitRemoteVideo);
  window.visualViewport?.addEventListener('resize', fitRemoteVideo);
});

onBeforeUnmount(() => {
  window.removeEventListener('resize', fitRemoteVideo);
  window.visualViewport?.removeEventListener('resize', fitRemoteVideo);
  stopSession();
});
</script>

<template>
  <main>
    <ConnectPanel
      v-model:invite="invite"
      v-model:fps="fps"
      v-model:bitrate="bitrate"
      v-model:scale="scale"
      :running="running"
      :supported="supported"
      @connect="connect"
      @stop="stopSession"
    />
    <RemoteViewer
      ref="viewer"
      :frame-visible="frameVisible"
      :control-active="controlActive"
      :status="status"
      :status-error="statusError"
      :canvas-style="canvasStyle"
      :cursor-style="cursorStyle"
      @capture="captureInput"
      @stop="stopSession"
    />
    <div class="telemetry">
      <strong id="status" :class="{ error: statusError }">{{ status }}</strong>
      <span id="details">{{ details }}</span>
    </div>
  </main>
</template>
