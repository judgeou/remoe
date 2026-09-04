import {
  FRAME_FLAG_KEY,
  SIGNAL_TYPE,
  SignalFrameBuffer,
  VideoDecodeGate,
  VideoFrameAssembler,
  av1CodecString,
  h264CodecString,
  MAGIC,
  decodeClipboardText,
  decodeClockSyncResponse,
  decodeCursorState,
  decodeStreamHeader,
  decodeStreamStatus,
  encodeClientConfig,
  encodeClockSyncRequest,
  encodeKeyFrameRequest,
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
  #video = null;
  #remoteTrack = null;
  #streamHeader = null;
  #decoder = null;
  #decoderConfig = null;
  #assembler = new VideoFrameAssembler();
  #decodeGate = new VideoDecodeGate();
  #firstFrameNotified = false;
  #decodeReceiveTimes = new Map();
  #statsStartedAt = 0;
  #statsBytes = 0;
  #statsFrames = 0;
  #statsLossEvents = 0;
  #statsDecodeMs = 0;
  #statsReceiveToPresentMs = 0;
  #statsCaptureToReceiveMs = 0;
  #statsEndToEndMs = 0;
  #statsPresentedFrames = 0;
  #statsCaptureTimingFrames = 0;
  #statsEndToEndFrames = 0;
  #clockTimer = 0;
  #clockSequence = 0;
  #clockPending = new Map();
  #clockSamples = [];
  #hostMinusClientUs = null;
  #signalBuffer = new SignalFrameBuffer();
  #pendingCandidates = [];
  #remoteDescription = false;
  #controlOpen = false;
  #videoTrackReady = false;
  #videoDataOpen = false;
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
  #lastKeyFrameRequest = Number.NEGATIVE_INFINITY;

  /**
   * @param {object} invite
   * @param {object} settings
   * @param {object} events
   */
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
    if (!('VideoDecoder' in globalThis)) throw new Error('浏览器不支持 WebCodecs VideoDecoder');
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
    if (this.#clockTimer) clearInterval(this.#clockTimer);
    try { this.#decoder?.close(); } catch { /* already closed */ }
    try { this.#control?.close(); } catch { /* already closed */ }
    try { this.#video?.close(); } catch { /* already closed */ }
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

    this.#video = peer.createDataChannel('remoe-video', {
      ordered: false,
      maxRetransmits: 0,
    });
    this.#video.binaryType = 'arraybuffer';
    this.#video.onopen = () => {
      this.#videoDataOpen = true;
      this.#maybeStartStream();
    };
    this.#video.onmessage = (event) => this.#handleVideo(event.data);
    this.#video.onerror = () => this.#fail(new Error('低延迟视频 DataChannel 出错'));
    this.#video.onclose = () => {
      if (!this.#stopped) this.#fail(new Error('低延迟视频 DataChannel 已关闭'));
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
    if (!this.#bootstrapComplete || !this.#videoTrackReady ||
        !this.#videoDataOpen || this.#streamStarted) return;
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
    event.track.onended = () => {
      if (!this.#stopped) this.#fail(new Error('远端视频 Track 已结束'));
    };
    this.#videoTrackReady = true;
    this.#maybeStartStream();
  }

  async #handleControl(value) {
    const bytes = toBytes(value);
    const magic = bytes.byteLength >= 4
      ? new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true)
      : 0;
    if (magic === MAGIC.clipboard) {
      const clipboard = decodeClipboardText(bytes);
      this.#events.onClipboard?.(clipboard.text);
      return;
    }
    if (magic === MAGIC.streamStatus) {
      this.#events.onStreamStatus?.(decodeStreamStatus(bytes));
      return;
    }
    if (magic === MAGIC.cursorState) {
      this.#events.onCursorState?.(decodeCursorState(bytes));
      return;
    }
    if (magic === MAGIC.clockSync) {
      this.#handleClockSync(bytes);
      return;
    }
    const header = decodeStreamHeader(bytes);
    if (this.#streamHeader) throw new Error('Host 发来了多个 StreamHeader');
    const isH264 = header.codec === MAGIC.h264;
    const codec = isH264 ? h264CodecString(header) : av1CodecString(header);
    const config = {
      codec,
      codedWidth: header.width,
      codedHeight: header.height,
      hardwareAcceleration: 'prefer-hardware',
      optimizeForLatency: true,
    };
    const support = await VideoDecoder.isConfigSupported(config);
    if (!support.supported) throw new Error(`浏览器不支持此视频配置：${codec}`);
    this.#decoder = new VideoDecoder({
      output: (frame) => {
        let frameTransferred = false;
        try {
          const now = performance.now();
          const timing = this.#decodeReceiveTimes.get(frame.timestamp);
          this.#decodeReceiveTimes.delete(frame.timestamp);
          if (timing) {
            this.#statsDecodeMs += now - timing.receivedAt;
            if (this.#hostMinusClientUs !== null) {
              const captureToReceive = (timing.receivedAt * 1_000 +
                this.#hostMinusClientUs - timing.hostTimestampUs) / 1_000;
              if (captureToReceive >= 0 && captureToReceive <= 60_000) {
                this.#statsCaptureToReceiveMs += captureToReceive;
                this.#statsCaptureTimingFrames += 1;
              }
            }
          }
          const onPresented = timing ? (presentedAt) => {
            this.#statsReceiveToPresentMs += presentedAt - timing.receivedAt;
            this.#statsPresentedFrames += 1;
            if (this.#hostMinusClientUs === null) return;
            const endToEnd = (presentedAt * 1_000 +
              this.#hostMinusClientUs - timing.hostTimestampUs) / 1_000;
            if (endToEnd >= 0 && endToEnd <= 60_000) {
              this.#statsEndToEndMs += endToEnd;
              this.#statsEndToEndFrames += 1;
            }
          } : undefined;
          if (this.#events.onFrame) {
            frameTransferred = true;
            this.#events.onFrame(frame, header, onPresented);
          }
          this.#statsFrames += 1;
          if (!this.#firstFrameNotified) {
            this.#firstFrameNotified = true;
            this.#events.onFirstFrame?.(header);
          }
          this.#maybeReportStats();
        } finally {
          if (!frameTransferred) frame.close();
        }
      },
      error: (error) => this.#fail(new Error(`WebCodecs 解码失败：${error.message}`)),
    });
    this.#decoderConfig = support.config ?? config;
    this.#decoder.configure(this.#decoderConfig);
    this.#streamHeader = header;
    this.#statsStartedAt = performance.now();
    this.#events.onStream?.({ ...header, codec, lowLatency: true });
    this.#control.send(encodeStreamReady());
    this.#inputReady = true;
    this.#sendClockSync();
    this.#clockTimer = setInterval(() => this.#sendClockSync(), 2_000);
    this.#status(`等待第一张 ${isH264 ? 'H.264' : 'AV1'} 画面…`);
  }

  #handleVideo(value) {
    try {
      const bytes = toBytes(value);
      this.#statsBytes += bytes.byteLength;
      const { frame, lossDetected } = this.#assembler.consume(bytes);
      const decision = this.#decodeGate.evaluate(frame, lossDetected);
      if (decision.recoveryStarted) {
        this.#statsLossEvents += 1;
        this.#assembler.clear();
        this.#decodeReceiveTimes.clear();
        this.#decoder?.reset();
        if (this.#decoder && this.#decoderConfig) this.#decoder.configure(this.#decoderConfig);
        this.#requestKeyFrame();
      }
      this.#maybeReportStats();
      if (!decision.frame || !this.#decoder) return;
      if (this.#decoder.decodeQueueSize > 2) {
        this.#statsLossEvents += 1;
        this.#assembler.clear();
        this.#decodeGate = new VideoDecodeGate();
        this.#decodeReceiveTimes.clear();
        this.#decoder.reset();
        this.#decoder.configure(this.#decoderConfig);
        this.#requestKeyFrame();
        return;
      }
      const timestamp = Number(decision.frame.timestampUs);
      this.#decodeReceiveTimes.set(timestamp, {
        receivedAt: performance.now(),
        hostTimestampUs: timestamp,
      });
      this.#decoder.decode(new EncodedVideoChunk({
        type: (decision.frame.flags & FRAME_FLAG_KEY) ? 'key' : 'delta',
        timestamp,
        data: decision.frame.data,
      }));
    } catch (error) {
      this.#fail(error);
    }
  }

  #maybeReportStats() {
    if (!this.#statsStartedAt) return;
    const now = performance.now();
    const elapsed = now - this.#statsStartedAt;
    if (elapsed < 1_000) return;
    const average = (total) => this.#statsFrames > 0 ? total / this.#statsFrames : 0;
    const presentedAverage = (total) => this.#statsPresentedFrames > 0
      ? total / this.#statsPresentedFrames : 0;
    const captureAverage = (total) => this.#statsCaptureTimingFrames > 0
      ? total / this.#statsCaptureTimingFrames : 0;
    const endToEndAverage = (total) => this.#statsEndToEndFrames > 0
      ? total / this.#statsEndToEndFrames : 0;
    this.#events.onStats?.({
      fps: this.#statsFrames * 1_000 / elapsed,
      bitrateMbps: this.#statsBytes * 8 / elapsed / 1_000,
      dataRateKBps: this.#statsBytes / elapsed,
      lostPackets: this.#statsLossEvents,
      droppedFrames: this.#statsLossEvents,
      jitterBufferMs: 0,
      jitterMinimumMs: 0,
      jitterTargetMs: 0,
      endToEndMs: endToEndAverage(this.#statsEndToEndMs),
      captureToReceiveMs: captureAverage(this.#statsCaptureToReceiveMs),
      receiveToPresentMs: presentedAverage(this.#statsReceiveToPresentMs),
      decodeMs: average(this.#statsDecodeMs),
      processingMs: presentedAverage(this.#statsReceiveToPresentMs),
      decoderImplementation: 'WebCodecs low-latency',
      powerEfficientDecoder: null,
    });
    this.#statsStartedAt = now;
    this.#statsBytes = 0;
    this.#statsFrames = 0;
    this.#statsLossEvents = 0;
    this.#statsDecodeMs = 0;
    this.#statsReceiveToPresentMs = 0;
    this.#statsCaptureToReceiveMs = 0;
    this.#statsEndToEndMs = 0;
    this.#statsPresentedFrames = 0;
    this.#statsCaptureTimingFrames = 0;
    this.#statsEndToEndFrames = 0;
  }

  #sendClockSync() {
    if (this.#control?.readyState !== 'open' || !this.#inputReady) return;
    const clientSendUs = performance.now() * 1_000;
    const sequence = this.#clockSequence++ >>> 0;
    this.#clockPending.set(sequence, clientSendUs);
    while (this.#clockPending.size > 8) this.#clockPending.delete(this.#clockPending.keys().next().value);
    this.#control.send(encodeClockSyncRequest(sequence, clientSendUs));
  }

  #handleClockSync(bytes) {
    const response = decodeClockSyncResponse(bytes);
    const clientReceiveUs = performance.now() * 1_000;
    const clientSendUs = this.#clockPending.get(response.sequence);
    this.#clockPending.delete(response.sequence);
    if (!Number.isFinite(clientSendUs) ||
        Math.abs(clientSendUs - response.clientSendUs) > 2) return;
    const rttUs = (clientReceiveUs - clientSendUs) -
      (response.hostSendUs - response.hostReceiveUs);
    if (rttUs < 0) return;
    const offsetUs = ((response.hostReceiveUs - clientSendUs) +
      (response.hostSendUs - clientReceiveUs)) / 2;
    this.#clockSamples.push({ rttUs, offsetUs });
    if (this.#clockSamples.length > 8) this.#clockSamples.shift();
    this.#hostMinusClientUs = this.#clockSamples.reduce((best, sample) =>
      sample.rttUs < best.rttUs ? sample : best).offsetUs;
  }

  #requestKeyFrame() {
    const now = performance.now();
    if (now - this.#lastKeyFrameRequest < 500 || this.#control?.readyState !== 'open') return;
    this.#lastKeyFrameRequest = now;
    this.#control.send(encodeKeyFrameRequest(this.#inputSequence++));
  }

  #sendSignal(type, value = '', metadata = '') {
    if (this.#websocket?.readyState !== WebSocket.OPEN) throw new Error('WSS 信令连接未打开');
    this.#websocket.send(encodeSignal(type, value, metadata));
  }

  #status(message) {
    // this.#events.onStatus?.(message);
  }

  #fail(value) {
    if (this.#stopped) return;
    const error = value instanceof Error ? value : new Error(String(value));
    this.stop();
    this.#events.onError?.(error);
  }
}
