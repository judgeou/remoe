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
import { ClipboardSynchronizer } from './core/clipboard-sync.js';
import { RemoteInputController } from './core/input.js';
import { LatestFrameRenderer } from './core/latest-frame-renderer.js';
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

interface HostCursorState extends CursorPosition {
  visible: boolean;
  embeddedInVideo: boolean;
  insideOutput: boolean;
}

type ViewportGesture =
  | { type: 'pan'; deltaX: number; deltaY: number }
  | { type: 'pinch'; scale: number; clientX: number; clientY: number;
      deltaX: number; deltaY: number };

interface PerformanceStats {
  fps: number;
  requestedBitrateMbps: number;
  hostBitrateMbps: number | null;
  pacingBitrateMbps: number | null;
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

interface RemoteWindowPayload {
  invite: string;
  fps: number;
  bitrate: number;
  rateControl: 'cbr' | 'fixed-quality';
  quality: number;
  scale: number;
}

type LockableScreenOrientation = ScreenOrientation & {
  lock?: (orientation: 'landscape') => Promise<void>;
};

const viewer = ref<InstanceType<typeof RemoteViewer> | null>(null);
const remoteWindowMode = new URLSearchParams(location.search).get('remote') === '1';
const remoteWindowPayloadPrefix = 'remoe-remote:';
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
const videoStyle = reactive<CSSProperties>({});
const cursorStyle = reactive<CSSProperties>({});
const performanceStats = reactive<PerformanceStats>({
  fps: 0,
  requestedBitrateMbps: 0,
  hostBitrateMbps: null,
  pacingBitrateMbps: null,
  bitrateMbps: 0,
  dataRateKBps: 0,
  lostPackets: 0,
  droppedFrames: 0,
  endToEndMs: 0,
  captureToReceiveMs: 0,
  receiveToPresentMs: 0,
  jitterBufferMs: 0,
  jitterMinimumMs: 0,
  jitterTargetMs: 0,
  decodeMs: 0,
  processingMs: 0,
  decoderImplementation: '',
  powerEfficientDecoder: null,
});
const touchMode = ref<'trackpad' | 'direct'>('trackpad');
const activeModifiers = ref<string[]>([]);
const fullscreenActive = ref(false);
const orientationLocked = ref(false);
const wakeLockEnabled = ref(false);
const client = shallowRef<RemoeBrowserClient | null>(null);
let inputController: RemoteInputController | null = null;
let clipboardSynchronizer: ClipboardSynchronizer | null = null;
let frameRenderer: LatestFrameRenderer | null = null;
let wakeLockSentinel: WakeLockSentinel | null = null;
let streamSize: { width: number; height: number } | null = null;
let fittedVideoSize: { width: number; height: number } | null = null;
let viewportPan = { x: 0, y: 0 };
let cursorPosition: CursorPosition = { x: 32768, y: 32768 };
let hostCursorState: HostCursorState = {
  x: 32768, y: 32768, visible: false, embeddedInVideo: false, insideOutput: false,
};
let accountRefreshTimer: number | null = null;

function video(): HTMLCanvasElement {
  if (!viewer.value) throw new Error('远程画面尚未挂载');
  return viewer.value.getVideo();
}

function setStatus(message: string, isError = false) {
  // status.value = message;
  // statusError.value = isError;
}

function createRemoteWindow(): Window | null {
  const popupScreen = window.screen as Screen & { availLeft?: number; availTop?: number };
  const width = Math.max(320, popupScreen.availWidth);
  const height = Math.max(240, popupScreen.availHeight);
  const left = popupScreen.availLeft ?? 0;
  const top = popupScreen.availTop ?? 0;
  const features = [
    'popup=yes',
    'toolbar=no',
    'location=no',
    'menubar=no',
    'status=no',
    'scrollbars=no',
    'resizable=yes',
    `width=${width}`,
    `height=${height}`,
    `left=${left}`,
    `top=${top}`,
  ].join(',');
  const popup = window.open('about:blank', '_blank', features);
  if (!popup) return null;
  try {
    popup.moveTo(left, top);
    popup.resizeTo(width, height);
  } catch {
    // Browsers may ignore window sizing outside a desktop popup context.
  }
  return popup;
}

function remoteWindowPayload(inviteUrl: string): RemoteWindowPayload {
  return {
    invite: inviteUrl,
    fps: fps.value,
    bitrate: bitrate.value,
    rateControl: rateControl.value,
    quality: quality.value,
    scale: scale.value,
  };
}

function launchRemoteWindow(popup: Window, inviteUrl: string) {
  const target = new URL(location.href);
  target.search = '';
  target.searchParams.set('remote', '1');
  target.searchParams.set('_t', new Date().getTime().toString());
  target.hash = '';
  popup.name = `${remoteWindowPayloadPrefix}${JSON.stringify(remoteWindowPayload(inviteUrl))}`;
  popup.location.replace(target.href);
  popup.focus();
}

function readRemoteWindowPayload(): RemoteWindowPayload {
  if (!window.name.startsWith(remoteWindowPayloadPrefix)) {
    throw new Error('远程窗口缺少连接信息，请从设备列表重新连接');
  }
  const serialized = window.name.slice(remoteWindowPayloadPrefix.length);
  window.name = '';
  const payload = JSON.parse(serialized) as Partial<RemoteWindowPayload>;
  if (typeof payload.invite !== 'string' || typeof payload.fps !== 'number' ||
      typeof payload.bitrate !== 'number' ||
      (payload.rateControl !== 'cbr' && payload.rateControl !== 'fixed-quality') ||
      typeof payload.quality !== 'number' || typeof payload.scale !== 'number') {
    throw new Error('远程窗口的连接信息无效，请从设备列表重新连接');
  }
  return payload as RemoteWindowPayload;
}

function openInviteRemoteWindow() {
  try {
    const inviteUrl = invite.value.trim() || location.href;
    parseInvite(inviteUrl);
    const popup = createRemoteWindow();
    if (!popup) throw new Error('浏览器阻止了远程窗口，请允许本站弹出窗口后重试');
    launchRemoteWindow(popup, inviteUrl);
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), true);
  }
}

function setRemoteActive(active: boolean) {
  remoteActive.value = active;
  document.body.classList.toggle('remote-active', active);
  if (!active) document.body.classList.remove('control-active');
}

function positionRemoteCursor(position: CursorPosition = cursorPosition) {
  cursorPosition = position;
  const point = cursorViewportPosition(position.x, position.y, video().getBoundingClientRect());
  cursorStyle.left = `${point.left}px`;
  cursorStyle.top = `${point.top}px`;
}

function setRemoteCursorVisible(visible: boolean) {
  if (visible) delete cursorStyle.display;
  else cursorStyle.display = 'none';
}

function shouldOverlayHostCursor(state: HostCursorState) {
  return state.insideOutput && (!state.embeddedInVideo || !state.visible);
}

function handleHostCursor(state: HostCursorState) {
  hostCursorState = state;
  const target = viewer.value?.getVideo();
  if (target && document.pointerLockElement === target && inputController?.touchMode === null) {
    positionRemoteCursor(state);
    setRemoteCursorVisible(shouldOverlayHostCursor(state));
  }
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
    delete videoStyle.transform;
    delete videoStyle.transformOrigin;
  } else {
    videoStyle.transformOrigin = 'center center';
    videoStyle.transform = `translate3d(${viewportPan.x}px, ${viewportPan.y}px, 0) ` +
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
  videoStyle.width = `${fitted.width}px`;
  videoStyle.height = `${fitted.height}px`;
  fittedVideoSize = fitted;
  applyViewportTransform();
}

function leaveRemoteMode() {
  clipboardSynchronizer?.stop();
  clipboardSynchronizer = null;
  frameRenderer?.dispose();
  frameRenderer = null;
  inputController?.dispose();
  inputController = null;
  controlActive.value = false;
  activeModifiers.value = [];
  frameVisible.value = false;
  streamSize = null;
  fittedVideoSize = null;
  viewportZoom.value = 1;
  viewportPan = { x: 0, y: 0 };
  hostCursorState = {
    x: 32768, y: 32768, visible: false, embeddedInVideo: false, insideOutput: false,
  };
  delete cursorStyle.display;
  delete videoStyle.width;
  delete videoStyle.height;
  delete videoStyle.transform;
  delete videoStyle.transformOrigin;
  Object.assign(performanceStats, {
    fps: 0,
    requestedBitrateMbps: 0,
    hostBitrateMbps: null,
    pacingBitrateMbps: null,
    bitrateMbps: 0,
    dataRateKBps: 0,
    lostPackets: 0,
    droppedFrames: 0,
    endToEndMs: 0,
    captureToReceiveMs: 0,
    receiveToPresentMs: 0,
    jitterBufferMs: 0,
    jitterMinimumMs: 0,
    jitterTargetMs: 0,
    decodeMs: 0,
    processingMs: 0,
    decoderImplementation: '',
    powerEfficientDecoder: null,
  });
  document.body.classList.remove('touch-control-active');
  releaseMobileDisplayFeatures();
  setRemoteActive(false);
}

function setInputActive(active: boolean) {
  controlActive.value = active;
  document.body.classList.toggle('control-active', active);
  const touchActive = active && inputController?.touchMode !== null;
  document.body.classList.toggle('touch-control-active', touchActive);
  const target = viewer.value?.getVideo();
  const pointerLocked = active && target && document.pointerLockElement === target;
  if (pointerLocked) {
    positionRemoteCursor(hostCursorState);
    setRemoteCursorVisible(shouldOverlayHostCursor(hostCursorState));
  } else {
    setRemoteCursorVisible(touchActive);
  }
  // setStatus(active
  //   ? (touchActive ? '正在触控远程桌面' : '正在控制远程桌面 · 按 Esc 释放键鼠')
  //   : '画面已连接 · 可直接点击，或从工具栏接管键鼠');
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

function stopRemoteWindowSession() {
  stopSession();
  if (remoteWindowMode) window.close();
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
        const target = video();
        target.width = stream.width;
        target.height = stream.height;
        const context = target.getContext('2d', { alpha: false });
        if (!context) throw new Error('浏览器无法创建 Canvas 2D context');
        frameRenderer?.dispose();
        frameRenderer = new LatestFrameRenderer((frame: VideoFrame) => {
          context.drawImage(frame, 0, 0, target.width, target.height);
        });
        clipboardSynchronizer?.stop();
        clipboardSynchronizer = new ClipboardSynchronizer(
          (text: string) => nextClient.sendClipboardText(text),
        );
        clipboardSynchronizer.start();
        performanceStats.requestedBitrateMbps = stream.bitrateBps / 1_000_000;
        const rate = stream.rateControl === 1
          ? `固定质量 ${stream.quality}`
          : `${(stream.bitrateBps / 1_000_000).toFixed(1)} Mbps CBR`;
        details.value = `${stream.width}×${stream.height} · ${stream.fpsNum} fps · ` +
          `${rate} · ${stream.codec}`;
      },
      onStreamStatus: (streamStatus: {
        mediaBitrateBps: number; pacingBitrateBps: number;
      }) => {
        performanceStats.hostBitrateMbps = streamStatus.mediaBitrateBps / 1_000_000;
        performanceStats.pacingBitrateMbps = streamStatus.pacingBitrateBps / 1_000_000;
      },
      onFrame: (frame: VideoFrame, _stream: StreamDescription,
        onPresented?: (presentedAt: number) => void) => {
        if (frameRenderer) frameRenderer.submit(frame, onPresented);
        else frame.close();
      },
      onFirstFrame: () => {
        const target = video();
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
            (position: CursorPosition) => {
              positionRemoteCursor(position);
              setRemoteCursorVisible(true);
            },
            (gesture: ViewportGesture) => handleViewportGesture(gesture),
          );
          target.focus({ preventScroll: true });
        });
        // setStatus('画面已连接 · 可直接点击，或从工具栏接管键鼠');
      },
      onStats: (stats: PerformanceStats) => Object.assign(performanceStats, stats),
      onFrameTiming: (timing: Pick<PerformanceStats,
        'endToEndMs' | 'captureToReceiveMs' | 'receiveToPresentMs'>) =>
        Object.assign(performanceStats, timing),
      onClipboard: (text: string) => { void clipboardSynchronizer?.receiveRemote(text); },
      onCursorState: (cursor: HostCursorState) => handleHostCursor(cursor),
      onError: (error: Error) => {
        leaveRemoteMode();
        setStatus(error.message, true);
        running.value = false;
      },
    });
    client.value = nextClient;
    await nextClient.connect();
  } catch (error) {
    stopSession();
    setStatus(error instanceof Error ? error.message : String(error), true);
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

async function connectManagedHost(id: string) {
  const popup = createRemoteWindow();
  if (!popup) {
    accountError.value = '浏览器阻止了远程窗口，请允许本站弹出窗口后重试';
    return;
  }
  return accountAction(async () => {
    try {
      const managedInvite = await requestHostConnection(id);
      launchRemoteWindow(popup, managedInvite);
    } catch (error) {
      popup.close();
      throw error;
    }
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
    await inputController?.capture(viewer.value?.getElement());
  } catch (error) {
    setStatus(`无法锁定鼠标：${error instanceof Error ? error.message : String(error)}`, true);
  }
}

onMounted(() => {
  if (!remoteWindowMode && location.hash.length > 1) invite.value = location.href;
  if (!globalThis.RTCPeerConnection || !globalThis.VideoDecoder) {
    supported.value = false;
    setStatus('当前浏览器不支持 WebRTC + WebCodecs 低延迟播放，请使用最新版 Chromium', true);
  }
  window.addEventListener('resize', fitRemoteVideo);
  window.visualViewport?.addEventListener('resize', fitRemoteVideo);
  document.addEventListener('fullscreenchange', handleFullscreenChange);
  document.addEventListener('visibilitychange', handleVisibilityChange);
  window.addEventListener('blur', handleWindowBlur);
  if (remoteWindowMode) {
    document.body.classList.add('remote-window');
    accountLoading.value = false;
    try {
      const payload = readRemoteWindowPayload();
      fps.value = payload.fps;
      bitrate.value = payload.bitrate;
      rateControl.value = payload.rateControl;
      quality.value = payload.quality;
      scale.value = payload.scale;
      setStatus('正在建立远程连接…');
      void nextTick(() => connect(payload.invite));
    } catch (error) {
      setStatus(error instanceof Error ? error.message : String(error), true);
    }
    return;
  }
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
  document.body.classList.remove('remote-window');
});
</script>

<template>
  <main>
    <p v-if="!remoteWindowMode && accountLoading" class="loading-state">正在载入账号…</p>
    <template v-else-if="!remoteWindowMode && !account.authenticated">
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
          @connect="openInviteRemoteWindow"
          @stop="stopSession"
        />
      </details>
    </template>
    <template v-else-if="!remoteWindowMode">
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
      :video-style="videoStyle"
      :cursor-style="cursorStyle"
      :performance-stats="performanceStats"
      :touch-mode="touchMode"
      :active-modifiers="activeModifiers"
      :fullscreen-active="fullscreenActive"
      :orientation-locked="orientationLocked"
      :wake-lock-enabled="wakeLockEnabled"
      :viewport-zoom="viewportZoom"
      @capture="captureInput"
      @stop="stopRemoteWindowSession"
      @touch-mode="selectTouchMode"
      @virtual-key="sendVirtualKey"
      @virtual-modifier="toggleVirtualModifier"
      @virtual-mouse="sendVirtualMouse"
      @text-input="sendTextInput"
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
