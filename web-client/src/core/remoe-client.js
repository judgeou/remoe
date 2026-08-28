import {
  SIGNAL_TYPE,
  SignalFrameBuffer,
  av1CodecString,
  h264CodecString,
  MAGIC,
  decodeClipboardText,
  decodeStreamHeader,
  encodeClientConfig,
  encodeInputEvent,
  encodeClipboardText,
  encodeSignal,
  encodeStreamReady,
} from './protocol.js';

const registrationErrors = Object.freeze({
  'error:invite-not-found': '邀请不存在、已经过期，或 Host 不在线',
  'error:invite-in-use': '这个邀请已经有 Client 在使用',
  'error:service-unavailable': '信令服务器当前不可用',
});

function toBytes(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  throw new TypeError('预期收到二进制消息');
}

export function parseInvite(input, pageLocation = globalThis.location) {
  const trimmed = input.trim();
  let url;
  if (trimmed) {
    url = new URL(trimmed);
    if (url.protocol === 'https:' || url.protocol === 'http:') {
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      url.pathname = '/signal';
      url.search = '';
    }
  } else {
    url = new URL(pageLocation.href);
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    url.pathname = '/signal';
    url.search = '';
  }
  if (url.protocol !== 'wss:' && url.protocol !== 'ws:') {
    throw new Error('邀请必须是 ws(s):// 或 http(s):// URL');
  }
  const session = url.hash.slice(1);
  if (!/^[A-Za-z0-9_-]{8,64}$/.test(session)) {
    throw new Error("邀请 URL 缺少有效的 '#session'");
  }
  url.hash = '';
  const websocket = new URL(url);
  websocket.searchParams.set('session', session);
  websocket.searchParams.set('role', 'client');
  return {
    websocket: websocket.href,
    stun: `stun:${url.hostname}:3478`,
    session,
  };
}

export class RemoeBrowserClient {
  #invite;
  #settings;
  #events;
  #websocket = null;
  #peer = null;
  #control = null;
  #remoteVideo = null;
  #remoteStream = null;
  #remoteTrack = null;
  #streamHeader = null;
  #videoFrameCallback = 0;
  #statsTimer = 0;
  #lastInboundStats = null;
  #signalBuffer = new SignalFrameBuffer();
  #pendingCandidates = [];
  #remoteDescription = false;
  #controlOpen = false;
  #videoTrackReady = false;
  #gatheringComplete = false;
  #remoteReady = false;
  #ackSent = false;
  #remoteAcknowledged = false;
  #bootstrapComplete = false;
  #streamStarted = false;
  #stopped = false;
  #inputSequence = 0;
  #inputReady = false;
  #clipboardSequence = 0;

  constructor(invite, settings, events = {}) {
    this.#invite = invite;
    this.#settings = settings;
    this.#events = events;
  }

  async connect() {
    if (!globalThis.isSecureContext && globalThis.location?.hostname !== 'localhost') {
      throw new Error('网页客户端必须运行在 HTTPS 安全上下文中');
    }
    if (!('RTCPeerConnection' in globalThis)) throw new Error('浏览器不支持 WebRTC');
    this.#status('正在注册邀请…');
    await this.#connectWebSocket();
    this.#createPeerConnection();
    this.#status('正在交换 SDP/ICE…');
    const offer = await this.#peer.createOffer();
    await this.#peer.setLocalDescription(offer);
    this.#sendSignal(SIGNAL_TYPE.description, offer.sdp, offer.type);
  }

  stop() {
    if (this.#stopped) return;
    this.#stopped = true;
    if (this.#statsTimer) clearInterval(this.#statsTimer);
    if (this.#videoFrameCallback && this.#remoteVideo?.cancelVideoFrameCallback) {
      this.#remoteVideo.cancelVideoFrameCallback(this.#videoFrameCallback);
    }
    if (this.#remoteVideo) this.#remoteVideo.srcObject = null;
    for (const track of this.#remoteStream?.getTracks?.() ?? []) track.stop();
    try { this.#control?.close(); } catch { /* already closed */ }
    try { this.#peer?.close(); } catch { /* already closed */ }
    try { this.#websocket?.close(1000, 'Client stopped'); } catch { /* already closed */ }
    this.#status('已停止');
  }

  sendInput(event) {
    if (!this.#inputReady || this.#control?.readyState !== 'open') return false;
    this.#control.send(encodeInputEvent({ ...event, sequence: this.#inputSequence++ }));
    return true;
  }

  sendClipboardText(text) {
    if (!this.#inputReady || this.#control?.readyState !== 'open') return false;
    this.#control.send(encodeClipboardText(text, this.#clipboardSequence++));
    return true;
  }

  async #connectWebSocket() {
    await new Promise((resolve, reject) => {
      let settled = false;
      const fail = (error) => {
        if (!settled) {
          settled = true;
          reject(error instanceof Error ? error : new Error(String(error)));
        } else this.#fail(error);
      };
      const socket = new WebSocket(this.#invite.websocket);
      this.#websocket = socket;
      socket.binaryType = 'arraybuffer';
      socket.onmessage = (event) => {
        if (typeof event.data === 'string') {
          if (event.data === 'registered' && !settled) {
            settled = true;
            resolve();
          } else if (registrationErrors[event.data]) fail(new Error(registrationErrors[event.data]));
          else if (event.data !== 'registered') fail(new Error('信令服务器返回了未知响应'));
          return;
        }
        if (!settled) return fail(new Error('信令服务器未先确认邀请注册'));
        this.#handleSignalBytes(event.data).catch((error) => this.#fail(error));
      };
      socket.onerror = () => fail(new Error('无法连接 WSS 信令服务器'));
      socket.onclose = (event) => {
        if (!this.#stopped && !this.#bootstrapComplete) {
          fail(new Error(event.reason || 'WebSocket 在 WebRTC 握手完成前关闭'));
        }
      };
    });
  }

  #createPeerConnection() {
    const peer = new RTCPeerConnection({
      iceServers: [{ urls: this.#invite.stun }],
      bundlePolicy: 'max-bundle',
    });
    this.#peer = peer;
    peer.onicecandidate = ({ candidate }) => {
      if (candidate) this.#sendSignal(SIGNAL_TYPE.candidate, candidate.candidate, candidate.sdpMid ?? '0');
      else {
        this.#gatheringComplete = true;
        this.#maybeAcknowledge();
      }
    };
    peer.oniceconnectionstatechange = () => {
      this.#events.onIceState?.(peer.iceConnectionState);
      if (peer.iceConnectionState === 'failed') this.#fail(new Error('ICE 直连失败；当前没有 TURN 回退'));
    };
    peer.onconnectionstatechange = () => {
      if (peer.connectionState === 'failed') this.#fail(new Error('WebRTC PeerConnection 失败'));
    };
    peer.ontrack = (event) => this.#attachRemoteTrack(event);

    const transceiver = peer.addTransceiver('video', { direction: 'recvonly' });
    const capabilities = globalThis.RTCRtpReceiver?.getCapabilities?.('video');
    if (capabilities?.codecs && transceiver.setCodecPreferences) {
      const codecs = capabilities.codecs.filter(({ mimeType }) =>
        /video\/(AV1|H264)/i.test(mimeType));
      if (codecs.length > 0) transceiver.setCodecPreferences(codecs);
    }

    this.#control = peer.createDataChannel('remoe-control', { ordered: true });
    this.#control.binaryType = 'arraybuffer';
    this.#control.onopen = () => {
      this.#controlOpen = true;
      this.#sendSignal(SIGNAL_TYPE.ready);
      this.#maybeAcknowledge();
    };
    this.#control.onmessage = (event) => this.#handleControl(event.data).catch((error) => this.#fail(error));
    this.#control.onerror = () => this.#fail(new Error('控制 DataChannel 出错'));
    this.#control.onclose = () => {
      if (!this.#stopped) this.#fail(new Error('控制 DataChannel 已关闭'));
    };
  }

  async #handleSignalBytes(value) {
    for (const frame of this.#signalBuffer.push(toBytes(value))) {
      if (frame.type === SIGNAL_TYPE.description) {
        if (this.#remoteDescription) throw new Error('Host 发来了多个 SDP description');
        await this.#peer.setRemoteDescription({ type: frame.metadata, sdp: frame.value });
        this.#remoteDescription = true;
        for (const candidate of this.#pendingCandidates) await this.#peer.addIceCandidate(candidate);
        this.#pendingCandidates = [];
      } else if (frame.type === SIGNAL_TYPE.candidate) {
        const candidate = { candidate: frame.value, sdpMid: frame.metadata || '0' };
        if (this.#remoteDescription) await this.#peer.addIceCandidate(candidate);
        else this.#pendingCandidates.push(candidate);
      } else if (frame.type === SIGNAL_TYPE.ready) {
        this.#remoteReady = true;
        this.#maybeAcknowledge();
      } else if (frame.type === SIGNAL_TYPE.acknowledged) {
        this.#remoteAcknowledged = true;
        this.#maybeFinishBootstrap();
      }
    }
  }

  #maybeAcknowledge() {
    if (!this.#ackSent && this.#controlOpen && this.#gatheringComplete && this.#remoteReady) {
      this.#ackSent = true;
      this.#sendSignal(SIGNAL_TYPE.acknowledged);
      this.#maybeFinishBootstrap();
    }
  }

  #maybeFinishBootstrap() {
    if (this.#bootstrapComplete || !this.#controlOpen || !this.#ackSent || !this.#remoteAcknowledged) return;
    this.#bootstrapComplete = true;
    this.#status('WebRTC 已连接，正在请求视频…');
    this.#maybeStartStream();
  }

  #maybeStartStream() {
    if (!this.#bootstrapComplete || !this.#videoTrackReady || this.#streamStarted) return;
    this.#streamStarted = true;
    this.#control.send(encodeClientConfig(this.#settings));
  }

  #attachRemoteTrack(event) {
    if (event.track.kind !== 'video') return;
    if (this.#remoteTrack && this.#remoteTrack !== event.track) {
      this.#fail(new Error('Host 发来了多个视频 Track'));
      return;
    }
    this.#remoteTrack = event.track;
    this.#remoteStream = event.streams[0] ?? new MediaStream([event.track]);
    const video = document.createElement('video');
    video.muted = true;
    video.autoplay = true;
    video.playsInline = true;
    video.srcObject = this.#remoteStream;
    this.#remoteVideo = video;
    event.track.onended = () => {
      if (!this.#stopped) this.#fail(new Error('远端视频 Track 已结束'));
    };
    this.#videoTrackReady = true;
    void video.play().catch((error) => this.#fail(
      new Error(`无法启动远端视频播放：${error.message}`)));
    this.#maybeStartStream();
  }

  async #handleControl(value) {
    const bytes = toBytes(value);
    if (bytes.byteLength >= 4 &&
        new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true) ===
          MAGIC.clipboard) {
      const clipboard = decodeClipboardText(bytes);
      this.#events.onClipboard?.(clipboard.text);
      return;
    }
    const header = decodeStreamHeader(bytes);
    const isH264 = header.codec === MAGIC.h264;
    const codec = isH264 ? h264CodecString(header) : av1CodecString(header);
    this.#streamHeader = header;
    this.#events.onStream?.({ ...header, codec });
    this.#control.send(encodeStreamReady());
    this.#inputReady = true;
    this.#startVideoFrames();
    this.#statsTimer = setInterval(() => {
      this.#reportPeerStats().catch((error) => this.#fail(error));
    }, 1_000);
    this.#status(`等待第一张 ${isH264 ? 'H.264' : 'AV1'} 画面…`);
  }

  #startVideoFrames() {
    if (!this.#remoteVideo || !this.#streamHeader) return;
    const video = this.#remoteVideo;
    if (!video.requestVideoFrameCallback) {
      this.#fail(new Error('浏览器不支持 requestVideoFrameCallback'));
      return;
    }
    const render = () => {
      if (this.#stopped) return;
      this.#events.onFrame?.(video, this.#streamHeader);
      this.#videoFrameCallback = video.requestVideoFrameCallback(render);
    };
    this.#videoFrameCallback = video.requestVideoFrameCallback(render);
  }

  async #reportPeerStats() {
    if (!this.#peer) return;
    const reports = await this.#peer.getStats(this.#remoteTrack ?? undefined);
    const inbound = [...reports.values()].find((report) =>
      report.type === 'inbound-rtp' && report.kind === 'video');
    if (!inbound) return;
    const current = {
      timestamp: inbound.timestamp,
      bytes: inbound.bytesReceived ?? 0,
      frames: inbound.framesDecoded ?? inbound.framesReceived ?? 0,
      lost: inbound.packetsLost ?? 0,
    };
    if (this.#lastInboundStats) {
      const elapsedMs = current.timestamp - this.#lastInboundStats.timestamp;
      if (elapsedMs > 0) {
        const bytes = Math.max(0, current.bytes - this.#lastInboundStats.bytes);
        this.#events.onStats?.({
          fps: Math.max(0, current.frames - this.#lastInboundStats.frames) * 1_000 / elapsedMs,
          bitrateMbps: bytes * 8 / elapsedMs / 1_000,
          dataRateKBps: bytes / elapsedMs,
          lossEvents: Math.max(0, current.lost - this.#lastInboundStats.lost),
        });
      }
    }
    this.#lastInboundStats = current;
  }

  #sendSignal(type, value = '', metadata = '') {
    if (this.#websocket?.readyState !== WebSocket.OPEN) throw new Error('WSS 信令连接未打开');
    this.#websocket.send(encodeSignal(type, value, metadata));
  }

  #status(message) {
    this.#events.onStatus?.(message);
  }

  #fail(value) {
    if (this.#stopped) return;
    const error = value instanceof Error ? value : new Error(String(value));
    this.stop();
    this.#events.onError?.(error);
  }
}
