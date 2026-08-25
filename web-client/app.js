import { RemoeBrowserClient, parseInvite } from './remoe-client.js?v=3';
import { RemoteInputController } from './input.js?v=3';
import { cursorViewportPosition, fitVideoSize } from './layout.js?v=3';

const form = document.querySelector('#connect-form');
const inviteInput = document.querySelector('#invite');
const connectButton = document.querySelector('#connect');
const stopButton = document.querySelector('#stop');
const status = document.querySelector('#status');
const details = document.querySelector('#details');
const canvas = document.querySelector('#video');
const placeholder = document.querySelector('#placeholder');
const controlGate = document.querySelector('#control-gate');
const captureButton = document.querySelector('#capture-input');
const remoteStopButton = document.querySelector('#remote-stop');
const remoteStatus = document.querySelector('#remote-status');
const remoteCursor = document.querySelector('#remote-cursor');
const context = canvas.getContext('2d', { alpha: false });
let client = null;
let inputController = null;
let firstFrame = true;
let streamSize = null;
let cursorPosition = { x: 32768, y: 32768 };

if (location.hash.length > 1) {
  inviteInput.placeholder = `使用当前页面邀请 #${location.hash.slice(1)}`;
}

function setStatus(message, isError = false) {
  status.textContent = message;
  remoteStatus.textContent = message;
  status.classList.toggle('error', isError);
  remoteStatus.classList.toggle('error', isError);
}

function setRunning(running) {
  connectButton.disabled = running;
  stopButton.disabled = !running;
  inviteInput.disabled = running;
}

function positionRemoteCursor(position = cursorPosition) {
  cursorPosition = position;
  const point = cursorViewportPosition(position.x, position.y, canvas.getBoundingClientRect());
  remoteCursor.style.left = `${point.left}px`;
  remoteCursor.style.top = `${point.top}px`;
}

function fitRemoteVideo() {
  if (!streamSize || !document.body.classList.contains('remote-active')) return;
  const fitted = fitVideoSize(
    streamSize.width, streamSize.height,
    document.documentElement.clientWidth, document.documentElement.clientHeight);
  canvas.style.width = `${fitted.width}px`;
  canvas.style.height = `${fitted.height}px`;
  positionRemoteCursor();
}

function leaveRemoteMode() {
  inputController?.dispose();
  inputController = null;
  document.body.classList.remove('remote-active', 'control-active');
  controlGate.hidden = true;
  streamSize = null;
  canvas.style.removeProperty('width');
  canvas.style.removeProperty('height');
}

function stopSession() {
  leaveRemoteMode();
  client?.stop();
  client = null;
  setRunning(false);
}

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    stopSession();
    firstFrame = true;
    details.textContent = '';
    placeholder.hidden = false;
    canvas.hidden = true;
    const invite = parseInvite(inviteInput.value);
    setRunning(true);
    client = new RemoeBrowserClient(invite, {
      fps: Number(document.querySelector('#fps').value),
      bitrateMbps: Number(document.querySelector('#bitrate').value),
      scalePercent: Number(document.querySelector('#scale').value),
    }, {
      onStatus: (message) => setStatus(message),
      onIceState: (state) => { details.textContent = `ICE: ${state}`; },
      onStream: (stream) => {
        streamSize = { width: stream.width, height: stream.height };
        canvas.width = stream.width;
        canvas.height = stream.height;
        document.body.classList.add('remote-active');
        fitRemoteVideo();
        details.textContent = `${stream.width}×${stream.height} · ${stream.fpsNum} fps · ` +
          `${(stream.bitrateBps / 1_000_000).toFixed(1)} Mbps · ${stream.codec}`;
      },
      onFrame: (frame) => {
        context.drawImage(frame, 0, 0, canvas.width, canvas.height);
        if (firstFrame) {
          firstFrame = false;
          canvas.hidden = false;
          placeholder.hidden = true;
          fitRemoteVideo();
          controlGate.hidden = false;
          inputController = new RemoteInputController(
            canvas,
            (input) => client?.sendInput(input) ?? false,
            (active) => {
              document.body.classList.toggle('control-active', active);
              controlGate.hidden = active;
              setStatus(active ? '正在控制远程桌面 · 按 Esc 释放键鼠' : '画面已连接 · 点击画面接管键鼠');
            },
            (position) => positionRemoteCursor(position));
          setStatus('画面已连接 · 点击画面接管键鼠');
        }
      },
      onError: (error) => {
        leaveRemoteMode();
        setStatus(error.message, true);
        setRunning(false);
      },
    });
    await client.connect();
  } catch (error) {
    setStatus(error.message, true);
    setRunning(false);
  }
});

stopButton.addEventListener('click', () => {
  stopSession();
});

remoteStopButton.addEventListener('click', stopSession);

window.addEventListener('resize', fitRemoteVideo);
window.visualViewport?.addEventListener('resize', fitRemoteVideo);

captureButton.addEventListener('click', async () => {
  try {
    await inputController?.capture();
  } catch (error) {
    setStatus(`无法锁定鼠标：${error.message}`, true);
  }
});

if (!globalThis.VideoDecoder) {
  setStatus('当前浏览器没有 WebCodecs VideoDecoder，请使用最新版 Chrome 或 Edge', true);
  connectButton.disabled = true;
}
