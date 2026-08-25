<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, reactive, ref, shallowRef, type CSSProperties } from 'vue';
import AccountPanel from './components/AccountPanel.vue';
import ConnectPanel from './components/ConnectPanel.vue';
import DevicePanel from './components/DevicePanel.vue';
import RemoteViewer from './components/RemoteViewer.vue';
import {
  addPasskey,
  connectHost as requestHostConnection,
  createAccount,
  deleteHost,
  getAccount,
  login,
  logout,
  pairHost,
  recoverAccount,
  renameHost,
  rotateRecoveryCode,
  type AccountState,
} from './api';
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
const account = reactive<AccountState>({ authenticated: false, hosts: [], passkeys: [] });
const accountLoading = ref(true);
const accountBusy = ref(false);
const accountError = ref('');
const recoveryCode = ref('');
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
let accountRefreshTimer: number | null = null;

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

async function connect(inviteOverride?: string) {
  try {
    stopSession();
    details.value = '';
    const parsedInvite = parseInvite(inviteOverride ?? invite.value);
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

async function refreshAccount() {
  try {
    const next = await getAccount();
    account.authenticated = next.authenticated;
    account.hosts = next.hosts;
    account.passkeys = next.passkeys;
  } catch (error) {
    accountError.value = error instanceof Error ? error.message : String(error);
  } finally {
    accountLoading.value = false;
  }
}

async function accountAction(action: () => Promise<void>) {
  accountBusy.value = true;
  accountError.value = '';
  try {
    await action();
  } catch (error) {
    accountError.value = error instanceof Error ? error.message : String(error);
  } finally {
    accountBusy.value = false;
  }
}

function registerAccount() {
  return accountAction(async () => {
    const result = await createAccount();
    recoveryCode.value = result.recoveryCode ?? '';
    await refreshAccount();
  });
}

function loginAccount() {
  return accountAction(async () => {
    await login();
    await refreshAccount();
  });
}

function recover(code: string) {
  return accountAction(async () => {
    const result = await recoverAccount(code);
    recoveryCode.value = result.recoveryCode ?? '';
    await refreshAccount();
  });
}

function logoutAccount() {
  return accountAction(async () => {
    await logout();
    recoveryCode.value = '';
    await refreshAccount();
  });
}

function pair(code: string, name: string) {
  return accountAction(async () => {
    await pairHost(code, name);
    await refreshAccount();
  });
}

function connectManagedHost(id: string) {
  return accountAction(async () => {
    const managedInvite = await requestHostConnection(id);
    await connect(managedInvite);
  });
}

function addAccountPasskey() {
  return accountAction(async () => {
    await addPasskey();
    await refreshAccount();
  });
}

function rotateRecovery() {
  return accountAction(async () => {
    recoveryCode.value = await rotateRecoveryCode();
  });
}

function updateHostName(id: string, name: string) {
  return accountAction(async () => {
    await renameHost(id, name);
    await refreshAccount();
  });
}

function removeHost(id: string) {
  return accountAction(async () => {
    await deleteHost(id);
    await refreshAccount();
  });
}

async function copyRecoveryCode() {
  await navigator.clipboard.writeText(recoveryCode.value);
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
  void refreshAccount();
  accountRefreshTimer = window.setInterval(() => {
    if (account.authenticated && !accountBusy.value && !running.value) void refreshAccount();
  }, 5_000);
});

onBeforeUnmount(() => {
  window.removeEventListener('resize', fitRemoteVideo);
  window.visualViewport?.removeEventListener('resize', fitRemoteVideo);
  if (accountRefreshTimer !== null) window.clearInterval(accountRefreshTimer);
  stopSession();
});
</script>

<template>
  <main>
    <p v-if="accountLoading" class="loading-state">正在载入账号…</p>
    <template v-else-if="!account.authenticated">
      <AccountPanel
        :busy="accountBusy"
        :error="accountError"
        @login="loginAccount"
        @create="registerAccount"
        @recover="recover"
      />
      <details class="legacy-connect">
        <summary>使用临时邀请 URL</summary>
        <ConnectPanel
          v-model:invite="invite"
          v-model:fps="fps"
          v-model:bitrate="bitrate"
          v-model:scale="scale"
          compact
          :running="running"
          :supported="supported"
          @connect="connect()"
          @stop="stopSession"
        />
      </details>
    </template>
    <template v-else>
      <aside v-if="recoveryCode" class="recovery-banner">
        <strong>请保存新的账号恢复码</strong>
        <code>{{ recoveryCode }}</code>
        <span>它只显示在当前页面；使用后会自动轮换。即使丢失，也可以在 Host 本机重新配对。</span>
        <div class="inline-actions">
          <button class="secondary small" @click="copyRecoveryCode">复制</button>
          <button class="text-button" @click="recoveryCode = ''">我已保存</button>
        </div>
      </aside>
      <DevicePanel
        v-model:fps="fps"
        v-model:bitrate="bitrate"
        v-model:scale="scale"
        :hosts="account.hosts"
        :passkeys="account.passkeys"
        :busy="accountBusy || running"
        :error="accountError"
        @connect="connectManagedHost"
        @refresh="refreshAccount"
        @logout="logoutAccount"
        @pair="pair"
        @add-passkey="addAccountPasskey"
        @rotate-recovery="rotateRecovery"
        @rename="updateHostName"
        @remove="removeHost"
      />
    </template>
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
