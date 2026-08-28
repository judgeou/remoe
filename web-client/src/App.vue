<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, reactive, ref, shallowRef, type CSSProperties } from 'vue';
import AccountPanel from './components/AccountPanel.vue';
import AndroidBindPanel from './components/AndroidBindPanel.vue';
import ConnectPanel from './components/ConnectPanel.vue';
import DevicePanel from './components/DevicePanel.vue';
import RemoteViewer from './components/RemoteViewer.vue';
import {
  addPasskey,
  authorizeNativeClient,
  connectHost as requestHostConnection,
  createAccount,
  deleteHost,
  getAccount,
  getNativeClientAuthorization,
  login,
  loginWithOtherPasskey,
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
  rateControl: number;
  quality: number;
  codec: string;
}

interface CursorPosition {
  x: number;
  y: number;
}

type ViewportGesture =
  | { type: 'pan'; deltaX: number; deltaY: number }
  | { type: 'pinch'; scale: number; clientX: number; clientY: number;
      deltaX: number; deltaY: number };

interface PerformanceStats {
  fps: number;
  bitrateMbps: number;
  dataRateKBps: number;
  lossEvents: number;
}

type LockableScreenOrientation = ScreenOrientation & {
  lock?: (orientation: 'landscape') => Promise<void>;
};

const viewer = ref<InstanceType<typeof RemoteViewer> | null>(null);
const account = reactive<AccountState>({ authenticated: false, accountId: null, hosts: [], passkeys: [] });
const accountLoading = ref(true);
const accountBusy = ref(false);
const accountError = ref('');
const recoveryCode = ref('');
const nativeClientCode = ref(new URLSearchParams(location.search).get('clientCode') ?? '');
const nativeClientAuthorized = ref(false);
const nativeClientName = ref('remoe Windows client');
const invite = ref('');
const fps = ref(60);
const bitrate = ref(20);
const rateControl = ref<'cbr' | 'fixed-quality'>('cbr');
const quality = ref(28);
const scale = ref(100);
const running = ref(false);
const supported = ref(true);
const status = ref('');
const statusError = ref(false);
const details = ref('');
const remoteActive = ref(false);
const controlActive = ref(false);
const frameVisible = ref(false);
const viewportZoom = ref(1);
const canvasStyle = reactive<CSSProperties>({});
const cursorStyle = reactive<CSSProperties>({});
const performanceStats = reactive<PerformanceStats>({
  fps: 0,
  bitrateMbps: 0,
  dataRateKBps: 0,
  lossEvents: 0,
});
const touchPreferred = ref(false);
const touchMode = ref<'trackpad' | 'direct'>('trackpad');
const activeModifiers = ref<string[]>([]);
const fullscreenActive = ref(false);
const orientationLocked = ref(false);
const wakeLockEnabled = ref(false);
const remoteClipboardPending = ref(false);
const client = shallowRef<RemoeBrowserClient | null>(null);
let inputController: RemoteInputController | null = null;
let wakeLockSentinel: WakeLockSentinel | null = null;
let streamSize: { width: number; height: number } | null = null;
let fittedVideoSize: { width: number; height: number } | null = null;
let viewportPan = { x: 0, y: 0 };
let cursorPosition: CursorPosition = { x: 32768, y: 32768 };
let accountRefreshTimer: number | null = null;
let remoteClipboardText = '';

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

function clampViewportPan() {
  if (!fittedVideoSize || !viewer.value || viewportZoom.value <= 1) {
    viewportPan = { x: 0, y: 0 };
    return;
  }
  const bounds = viewer.value.getElement().getBoundingClientRect();
  const maxX = Math.max(0, (fittedVideoSize.width * viewportZoom.value - bounds.width) / 2);
  const maxY = Math.max(0, (fittedVideoSize.height * viewportZoom.value - bounds.height) / 2);
  viewportPan.x = Math.max(-maxX, Math.min(maxX, viewportPan.x));
  viewportPan.y = Math.max(-maxY, Math.min(maxY, viewportPan.y));
}

function applyViewportTransform() {
  clampViewportPan();
  if (viewportZoom.value <= 1) {
    viewportZoom.value = 1;
    delete canvasStyle.transform;
    delete canvasStyle.transformOrigin;
  } else {
    canvasStyle.transformOrigin = 'center center';
    canvasStyle.transform = `translate3d(${viewportPan.x}px, ${viewportPan.y}px, 0) ` +
      `scale(${viewportZoom.value})`;
  }
  void nextTick(() => positionRemoteCursor());
}

function resetViewport() {
  viewportZoom.value = 1;
  viewportPan = { x: 0, y: 0 };
  applyViewportTransform();
}

function handleViewportGesture(gesture: ViewportGesture): boolean {
  if (gesture.type === 'pan') {
    if (viewportZoom.value <= 1) return false;
    viewportPan.x += gesture.deltaX;
    viewportPan.y += gesture.deltaY;
    applyViewportTransform();
    return true;
  }

  if (!Number.isFinite(gesture.scale) || gesture.scale <= 0 || !viewer.value) return true;
  const oldZoom = viewportZoom.value;
  const nextZoom = Math.max(1, Math.min(4, oldZoom * gesture.scale));
  const bounds = viewer.value.getElement().getBoundingClientRect();
  const previousCenterX = gesture.clientX - gesture.deltaX;
  const previousCenterY = gesture.clientY - gesture.deltaY;
  const ratio = nextZoom / oldZoom;
  viewportPan.x += gesture.deltaX +
    (1 - ratio) * (previousCenterX - (bounds.left + bounds.width / 2 + viewportPan.x));
  viewportPan.y += gesture.deltaY +
    (1 - ratio) * (previousCenterY - (bounds.top + bounds.height / 2 + viewportPan.y));
  viewportZoom.value = nextZoom;
  applyViewportTransform();
  return true;
}

function fitRemoteVideo() {
  if (!streamSize || !remoteActive.value) return;
  const viewportWidth = window.visualViewport?.width ?? document.documentElement.clientWidth;
  const viewportHeight = window.visualViewport?.height ?? document.documentElement.clientHeight;
  const fitted = fitVideoSize(
    streamSize.width,
    streamSize.height,
    viewportWidth,
    viewportHeight,
  );
  canvasStyle.width = `${fitted.width}px`;
  canvasStyle.height = `${fitted.height}px`;
  fittedVideoSize = fitted;
  applyViewportTransform();
}

function leaveRemoteMode() {
  inputController?.dispose();
  inputController = null;
  controlActive.value = false;
  activeModifiers.value = [];
  frameVisible.value = false;
  streamSize = null;
  fittedVideoSize = null;
  viewportZoom.value = 1;
  viewportPan = { x: 0, y: 0 };
  delete canvasStyle.width;
  delete canvasStyle.height;
  delete canvasStyle.transform;
  delete canvasStyle.transformOrigin;
  Object.assign(performanceStats, { fps: 0, bitrateMbps: 0, dataRateKBps: 0, lossEvents: 0 });
  remoteClipboardPending.value = false;
  remoteClipboardText = '';
  document.body.classList.remove('touch-control-active');
  releaseMobileDisplayFeatures();
  setRemoteActive(false);
}

function setInputActive(active: boolean) {
  controlActive.value = active;
  document.body.classList.toggle('control-active', active);
  const touchActive = active && inputController?.touchMode !== null;
  document.body.classList.toggle('touch-control-active', touchActive);
  setStatus(active
    ? (touchActive ? '正在触控远程桌面' : '正在控制远程桌面 · 按 Esc 释放键鼠')
    : (touchPreferred.value
      ? '画面已连接 · 点击画面开始触控'
      : '画面已连接 · 点击画面接管键鼠'));
}

function selectTouchMode(mode: 'trackpad' | 'direct') {
  touchMode.value = mode;
  inputController?.setTouchMode(mode);
  if (inputController) setInputActive(true);
}

function sendVirtualKey(code: string) {
  inputController?.tapKey(code);
}

function toggleVirtualModifier(code: string) {
  const active = activeModifiers.value.includes(code);
  inputController?.setModifier(code, !active);
  activeModifiers.value = active
    ? activeModifiers.value.filter((value) => value !== code)
    : [...activeModifiers.value, code];
}

function sendVirtualMouse(button: 'left' | 'right') {
  inputController?.tapMouseButton(button);
}

function sendTextInput(text: string) {
  const unsupported = inputController?.sendText(text) ?? [];
  if (unsupported.length > 0) {
    setStatus('移动软键盘目前仅支持英文、数字和常用符号', true);
  }
}

async function sendLocalClipboard() {
  try {
    if (!navigator.clipboard?.readText) throw new Error('当前浏览器不允许读取剪贴板');
    const text = await navigator.clipboard.readText();
    if (!client.value?.sendClipboardText(text)) throw new Error('控制连接尚未就绪');
    setStatus('本地剪贴板已发送到远程电脑');
  } catch (error) {
    setStatus(`无法发送剪贴板：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

async function acceptRemoteClipboard() {
  try {
    if (!navigator.clipboard?.writeText) throw new Error('当前浏览器不允许写入剪贴板');
    await navigator.clipboard.writeText(remoteClipboardText);
    remoteClipboardPending.value = false;
    setStatus('远程剪贴板已同步到本机');
  } catch (error) {
    setStatus(`无法接收剪贴板：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

async function handleRemoteClipboard(text: string) {
  remoteClipboardText = text;
  remoteClipboardPending.value = true;
  if (!document.hasFocus() || !navigator.clipboard?.writeText) return;
  try {
    await navigator.clipboard.writeText(text);
    remoteClipboardPending.value = false;
  } catch {
    // Browsers commonly require a user gesture. Keep the toolbar action available.
  }
}

async function enterFullscreen() {
  if (document.fullscreenElement) return;
  await viewer.value?.getElement().requestFullscreen({ navigationUI: 'hide' });
}

async function toggleFullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await enterFullscreen();
  } catch (error) {
    setStatus(`无法切换全屏：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

function unlockOrientation() {
  const orientation = screen.orientation as LockableScreenOrientation;
  orientation.unlock();
  orientationLocked.value = false;
}

async function toggleOrientation() {
  try {
    if (orientationLocked.value) {
      unlockOrientation();
      return;
    }
    const orientation = screen.orientation as LockableScreenOrientation;
    if (!orientation.lock) throw new Error('当前浏览器不支持锁定屏幕方向');
    await enterFullscreen();
    await orientation.lock('landscape');
    orientationLocked.value = true;
  } catch (error) {
    setStatus(`无法锁定横屏：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

async function requestWakeLock() {
  const wakeLock = navigator.wakeLock;
  if (!wakeLock) throw new Error('当前浏览器不支持屏幕常亮');
  wakeLockSentinel = await wakeLock.request('screen');
  wakeLockSentinel.addEventListener('release', () => { wakeLockSentinel = null; }, { once: true });
}

async function toggleWakeLock() {
  try {
    wakeLockEnabled.value = !wakeLockEnabled.value;
    if (wakeLockEnabled.value) await requestWakeLock();
    else {
      await wakeLockSentinel?.release();
      wakeLockSentinel = null;
    }
  } catch (error) {
    wakeLockEnabled.value = false;
    setStatus(`无法保持屏幕常亮：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

function releaseMobileDisplayFeatures() {
  wakeLockEnabled.value = false;
  void wakeLockSentinel?.release();
  wakeLockSentinel = null;
  unlockOrientation();
  if (document.fullscreenElement === viewer.value?.getElement()) void document.exitFullscreen();
}

function handleFullscreenChange() {
  fullscreenActive.value = document.fullscreenElement === viewer.value?.getElement();
  if (!fullscreenActive.value && orientationLocked.value) unlockOrientation();
}

function handleVisibilityChange() {
  if (document.hidden) activeModifiers.value = [];
  if (!document.hidden && wakeLockEnabled.value && !wakeLockSentinel) {
    void requestWakeLock().catch((error) => {
      wakeLockEnabled.value = false;
      setStatus(`无法恢复屏幕常亮：${error instanceof Error ? error.message : String(error)}`, true);
    });
  }
}

function handleWindowBlur() {
  activeModifiers.value = [];
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
      rateControl: rateControl.value,
      quality: quality.value,
      scalePercent: scale.value,
    }, {
      onStatus: (message: string) => setStatus(message),
      onIceState: (state: string) => { details.value = `ICE: ${state}`; },
      onStream: (stream: StreamDescription) => {
        streamSize = { width: stream.width, height: stream.height };
        const target = canvas();
        target.width = stream.width;
        target.height = stream.height;
        const rate = stream.rateControl === 1
          ? `固定质量 ${stream.quality} · 网络 ${(stream.bitrateBps / 1_000_000).toFixed(1)} Mbps`
          : `${(stream.bitrateBps / 1_000_000).toFixed(1)} Mbps CBR`;
        details.value = `${stream.width}×${stream.height} · ${stream.fpsNum} fps · ` +
          `${rate} · ${stream.codec}`;
      },
      onFrame: (frame: CanvasImageSource) => {
        const target = canvas();
        const context = target.getContext('2d', { alpha: false });
        if (!context) throw new Error('浏览器无法创建 Canvas 2D context');
        context.drawImage(frame, 0, 0, target.width, target.height);
        if (frameVisible.value) return;
        frameVisible.value = true;
        setRemoteActive(true);
        void nextTick(() => {
          fitRemoteVideo();
          inputController = new RemoteInputController(
            target,
            (input: { type: number; flags?: number; value1?: number; value2?: number }) =>
              client.value?.sendInput(input) ?? false,
            (active: boolean) => {
              setInputActive(active);
            },
            (position: CursorPosition) => positionRemoteCursor(position),
            (gesture: ViewportGesture) => handleViewportGesture(gesture),
          );
        });
        setStatus(touchPreferred.value
          ? '画面已连接 · 点击画面开始触控'
          : '画面已连接 · 点击画面接管键鼠');
      },
      onStats: (stats: PerformanceStats) => Object.assign(performanceStats, stats),
      onClipboard: (text: string) => { void handleRemoteClipboard(text); },
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
    account.accountId = next.accountId;
    account.hosts = next.hosts;
    account.passkeys = next.passkeys;
    if (next.authenticated && nativeClientCode.value && !nativeClientAuthorized.value) {
      const authorization = await getNativeClientAuthorization(nativeClientCode.value);
      nativeClientName.value = authorization.clientName;
    }
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
    showCredentialStorageWarning(result.credentialStored);
  });
}

function loginAccount() {
  return accountAction(async () => {
    const result = await login();
    await refreshAccount();
    showCredentialStorageWarning(result.credentialStored);
  });
}

function loginOtherAccount() {
  return accountAction(async () => {
    const result = await loginWithOtherPasskey();
    await refreshAccount();
    showCredentialStorageWarning(result.credentialStored);
  });
}

function showCredentialStorageWarning(stored: boolean) {
  if (!stored) {
    accountError.value = '此浏览器无法保存本机 passkey 标识；退出后可能无法再次登录，请保存最新恢复码。';
  }
}

function recover(code: string) {
  return accountAction(async () => {
    const result = await recoverAccount(code);
    recoveryCode.value = result.recoveryCode ?? '';
    await refreshAccount();
    showCredentialStorageWarning(result.credentialStored);
  });
}

function logoutAccount() {
  return accountAction(async () => {
    await logout();
    recoveryCode.value = '';
    await refreshAccount();
  });
}

function authorizeClient() {
  return accountAction(async () => {
    const result = await authorizeNativeClient(nativeClientCode.value);
    nativeClientAuthorized.value = result.authorized;
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
    const result = await addPasskey();
    await refreshAccount();
    showCredentialStorageWarning(result.credentialStored);
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
    if (touchPreferred.value) selectTouchMode(touchMode.value);
    else await inputController?.capture();
  } catch (error) {
    setStatus(`无法锁定鼠标：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

onMounted(() => {
  if (location.hash.length > 1) invite.value = location.href;
  if (!globalThis.RTCPeerConnection ||
      !HTMLVideoElement.prototype.requestVideoFrameCallback) {
    supported.value = false;
    setStatus('当前浏览器不支持标准 WebRTC 视频播放，请使用最新版浏览器', true);
  }
  window.addEventListener('resize', fitRemoteVideo);
  window.visualViewport?.addEventListener('resize', fitRemoteVideo);
  document.addEventListener('fullscreenchange', handleFullscreenChange);
  document.addEventListener('visibilitychange', handleVisibilityChange);
  window.addEventListener('blur', handleWindowBlur);
  touchPreferred.value = navigator.maxTouchPoints > 0 || matchMedia('(any-pointer: coarse)').matches;
  void refreshAccount();
  accountRefreshTimer = window.setInterval(() => {
    if (account.authenticated && !accountBusy.value && !running.value) void refreshAccount();
  }, 5_000);
});

onBeforeUnmount(() => {
  window.removeEventListener('resize', fitRemoteVideo);
  window.visualViewport?.removeEventListener('resize', fitRemoteVideo);
  document.removeEventListener('fullscreenchange', handleFullscreenChange);
  document.removeEventListener('visibilitychange', handleVisibilityChange);
  window.removeEventListener('blur', handleWindowBlur);
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
        @login-other="loginOtherAccount"
        @create="registerAccount"
        @recover="recover"
      />
      <details class="legacy-connect">
        <summary>使用临时邀请 URL</summary>
        <ConnectPanel
          v-model:invite="invite"
          v-model:fps="fps"
          v-model:bitrate="bitrate"
          v-model:rate-control="rateControl"
          v-model:quality="quality"
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
      <aside v-if="nativeClientCode && !nativeClientAuthorized" class="recovery-banner">
        <strong>授权 {{ nativeClientName }}</strong>
        <code>{{ nativeClientCode }}</code>
        <span>确认后，原生 Client 可以查看并连接当前账号下的远程电脑。</span>
        <div class="inline-actions">
          <button :disabled="accountBusy" @click="authorizeClient">允许此 Client</button>
        </div>
      </aside>
      <aside v-if="nativeClientAuthorized" class="recovery-banner">
        <strong>Client 已获授权</strong>
        <span>可以关闭此页面并返回 remoe_client。</span>
      </aside>
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
        v-model:rate-control="rateControl"
        v-model:quality="quality"
        v-model:scale="scale"
        :hosts="account.hosts"
        :passkeys="account.passkeys"
        :account-id="account.accountId"
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
      <AndroidBindPanel />
    </template>
    <RemoteViewer
      ref="viewer"
      :frame-visible="frameVisible"
      :control-active="controlActive"
      :status="status"
      :status-error="statusError"
      :canvas-style="canvasStyle"
      :cursor-style="cursorStyle"
      :performance-stats="performanceStats"
      :touch-preferred="touchPreferred"
      :touch-mode="touchMode"
      :active-modifiers="activeModifiers"
      :fullscreen-active="fullscreenActive"
      :orientation-locked="orientationLocked"
      :wake-lock-enabled="wakeLockEnabled"
      :remote-clipboard-pending="remoteClipboardPending"
      :viewport-zoom="viewportZoom"
      @capture="captureInput"
      @stop="stopSession"
      @touch-mode="selectTouchMode"
      @virtual-key="sendVirtualKey"
      @virtual-modifier="toggleVirtualModifier"
      @virtual-mouse="sendVirtualMouse"
      @text-input="sendTextInput"
      @send-clipboard="sendLocalClipboard"
      @receive-clipboard="acceptRemoteClipboard"
      @fullscreen="toggleFullscreen"
      @orientation="toggleOrientation"
      @wake-lock="toggleWakeLock"
      @reset-viewport="resetViewport"
    />
    <div v-if="status || details" class="telemetry">
      <strong id="status" :class="{ error: statusError }">{{ status }}</strong>
      <span id="details">{{ details }}</span>
    </div>
  </main>
</template>
