package top.ozaoza.remoe.rtc

import android.os.SystemClock
import android.view.Choreographer
import okhttp3.OkHttpClient
import org.webrtc.DataChannel
import org.webrtc.CandidatePairChangeEvent
import org.webrtc.IceCandidate
import org.webrtc.IceCandidateErrorEvent
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.RTCStatsReport
import org.webrtc.RtpReceiver
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.VideoTrack
import top.ozaoza.remoe.diagnostics.DiagnosticLog
import top.ozaoza.remoe.protocol.ClientConfig
import top.ozaoza.remoe.protocol.ClientConfigCodec
import top.ozaoza.remoe.protocol.ClipboardCodec
import top.ozaoza.remoe.protocol.InputEventCodec
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent
import top.ozaoza.remoe.protocol.SignalFrame
import top.ozaoza.remoe.protocol.SignalFrameBuffer
import top.ozaoza.remoe.protocol.SignalFrameCodec
import top.ozaoza.remoe.protocol.SignalType
import top.ozaoza.remoe.protocol.StreamHeader
import top.ozaoza.remoe.protocol.StreamHeaderCodec
import top.ozaoza.remoe.protocol.StreamReadyCodec
import top.ozaoza.remoe.signaling.ConnectionInvite
import top.ozaoza.remoe.signaling.SignalWebSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class RtcSession(
    private val runtime: RtcRuntime,
    httpClient: OkHttpClient,
    private val invite: ConnectionInvite,
    private val clientConfig: ClientConfig,
    private val diagnostics: DiagnosticLog,
    private val observer: Observer,
) {
    interface Observer {
        fun onStatus(message: String)
        fun onVideoTrack(track: VideoTrack)
        fun onStreamHeader(header: StreamHeader)
        fun onPerformanceStats(stats: RtcPerformanceStats)
        fun onDiagnostics(summary: String)
        fun onError(message: String)
    }

    private val signalBuffer = SignalFrameBuffer()
    private val pendingCandidates = mutableListOf<IceCandidate>()
    private val remoteMidToMLineIndex = mutableMapOf<String, Int>()
    private val statsExecutor = Executors.newSingleThreadScheduledExecutor()
    private val signalSocket = SignalWebSocket(
        httpClient,
        invite.websocketUrl,
        SignalListener(),
    )
    private var statsTask: ScheduledFuture<*>? = null
    private var peer: PeerConnection? = null
    private var control: DataChannel? = null
    private var remoteTrack: VideoTrack? = null
    private var remoteDescriptionSet = false
    private var controlOpen = false
    private var videoTrackReady = false
    private var gatheringComplete = false
    private var remoteReady = false
    private var acknowledgementSent = false
    private var remoteAcknowledged = false
    @Volatile
    private var bootstrapComplete = false
    private var streamRequested = false
    private var streamHeader: StreamHeader? = null
    private var localCandidateCount = 0
    private var remoteCandidateCount = 0
    private var lastIceStatsSummary: String? = null
    private val performanceStatsCalculator = RtcPerformanceStatsCalculator()
    private val receivedSignalCounts = mutableMapOf<SignalType, Int>()
    private var remoteSdpObserver: SdpObserver? = null
    private var inputSequence = 0u
    private val inputChoreographer = Choreographer.getInstance()
    private val inputFrameCallback = Choreographer.FrameCallback { flushPendingMove() }
    private var pendingMove: RemoteInputEvent? = null
    private var inputFrameScheduled = false
    @Volatile
    private var stopped = false
    @Volatile
    private var performanceStatsEnabled = false

    fun connect() {
        check(!stopped) { "session is stopped" }
        status("正在注册邀请…")
        diagnostics.append("signal", "Connecting to WSS host=${invite.displayHost}")
        signalSocket.connect()
    }

    @Synchronized
    fun sendInput(event: RemoteInputEvent): Boolean {
        if (stopped || !bootstrapComplete || streamHeader == null) return false
        if (event.type == InputType.MOUSE_MOVE) {
            // Pointer moves are absolute, so only the newest unsent position matters.
            pendingMove = event
            schedulePendingMove()
            return true
        }
        // Preserve ordering for button and wheel input, especially drag release.
        if (!flushPendingMove(force = true)) return false
        return sendInputNow(event)
    }

    @Synchronized
    fun setPerformanceStatsEnabled(enabled: Boolean) {
        if (stopped || performanceStatsEnabled == enabled) return
        performanceStatsEnabled = enabled
        performanceStatsCalculator.reset()
        if (enabled) {
            startStats()
        } else {
            statsTask?.cancel(false)
            statsTask = null
        }
    }

    @Synchronized
    fun close(reason: String = "已断开") {
        if (stopped) return
        stopped = true
        pendingMove = null
        if (inputFrameScheduled) {
            inputChoreographer.removeFrameCallback(inputFrameCallback)
            inputFrameScheduled = false
        }
        statsTask?.cancel(false)
        statsTask = null
        statsExecutor.shutdownNow()
        runCatching { control?.unregisterObserver() }
        runCatching { control?.close() }
        runCatching { control?.dispose() }
        control = null
        runCatching { peer?.close() }
        runCatching { peer?.dispose() }
        peer = null
        signalSocket.close()
        diagnostics.append("lifecycle", "RtcSession disposed: $reason")
        observer.onStatus(reason)
    }

    private fun createPeerConnection() {
        if (stopped) return
        val iceServer = PeerConnection.IceServer.builder(invite.stunUrl).createIceServer()
        val configuration = PeerConnection.RTCConfiguration(listOf(iceServer)).apply {
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            iceTransportsType = PeerConnection.IceTransportsType.ALL
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            enableIceGatheringOnAnyAddressPorts = true
        }
        diagnostics.append("ice", "Any-address candidate gathering enabled")
        val created = runtime.peerConnectionFactory.createPeerConnection(configuration, PeerObserver())
            ?: return fail("无法创建 PeerConnection")
        peer = created

        val transceiver = created.addTransceiver(
            MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
            RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.RECV_ONLY),
        )
        val preferredCodecs = runtime.peerConnectionFactory
            .getRtpReceiverCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
            .codecs
            .filter { it.mimeType.equals("video/AV1", true) || it.mimeType.equals("video/H264", true) }
        if (preferredCodecs.isNotEmpty()) {
            val result = transceiver.setCodecPreferences(preferredCodecs)
            diagnostics.append(
                "codec",
                "Set AV1/H264 preferences count=${preferredCodecs.size} success=${result.isSuccess}",
            )
        }

        val channel = created.createDataChannel("remoe-control", DataChannel.Init().apply {
            ordered = true
            maxRetransmits = -1
            maxRetransmitTimeMs = -1
        }) ?: return fail("无法创建 remoe-control DataChannel")
        control = channel
        channel.registerObserver(ControlObserver())

        status("正在交换 SDP/ICE…")
        created.createOffer(CreateOfferObserver(), MediaConstraints())
    }

    private inner class CreateOfferObserver : SimpleSdpObserver() {
        override fun onCreateSuccess(description: SessionDescription) {
            val currentPeer = peer ?: return
            currentPeer.setLocalDescription(object : SimpleSdpObserver() {
                override fun onSetSuccess() {
                    sendSignal(
                        SignalFrame(
                            SignalType.DESCRIPTION,
                            description.description,
                            description.type.canonicalForm(),
                        ),
                    )
                }

                override fun onSetFailure(error: String) = fail("设置 local SDP 失败：$error")
            }, description)
        }

        override fun onCreateFailure(error: String) = fail("创建 SDP offer 失败：$error")
    }

    private inner class PeerObserver : PeerConnection.Observer {
        override fun onSignalingChange(state: PeerConnection.SignalingState) {
            diagnostics.append("peer", "signaling=$state")
        }

        override fun onIceConnectionChange(state: PeerConnection.IceConnectionState) {
            diagnostics.append("ice", "connection=$state")
            observer.onDiagnostics("ICE: $state")
            if (state == PeerConnection.IceConnectionState.FAILED) {
                fail("ICE 直连失败；当前没有 TURN 回退")
            }
        }

        override fun onConnectionChange(state: PeerConnection.PeerConnectionState) {
            diagnostics.append("peer", "connection=$state")
            if (state == PeerConnection.PeerConnectionState.FAILED) fail("PeerConnection 失败")
        }

        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
            diagnostics.append("ice", "gathering=$state")
            if (state == PeerConnection.IceGatheringState.COMPLETE) {
                gatheringComplete = true
                maybeAcknowledge()
            }
        }

        override fun onIceCandidate(candidate: IceCandidate) {
            localCandidateCount += 1
            diagnostics.append(
                "ice",
                "local candidate #$localCandidateCount mid=${candidate.sdpMid ?: "none"} " +
                    "mline=${candidate.sdpMLineIndex} ${candidateSummary(candidate.sdp)}",
            )
            sendSignal(
                SignalFrame(
                    SignalType.CANDIDATE,
                    candidate.sdp,
                    candidate.sdpMid ?: "0",
                ),
            )
        }

        override fun onIceCandidateError(event: IceCandidateErrorEvent) {
            diagnostics.append(
                "ice",
                "candidate error code=${event.errorCode} urlScheme=${event.url.substringBefore(':')} text=${event.errorText}",
            )
        }

        override fun onSelectedCandidatePairChanged(event: CandidatePairChangeEvent) {
            diagnostics.append(
                "ice",
                "selected local=${candidateSummary(event.local.sdp)} remote=${candidateSummary(event.remote.sdp)} reason=${event.reason}",
            )
        }

        override fun onTrack(transceiver: RtpTransceiver) {
            handleRemoteTrack(transceiver.receiver.track())
        }

        override fun onAddTrack(receiver: RtpReceiver, mediaStreams: Array<out MediaStream>) {
            handleRemoteTrack(receiver.track())
        }

        override fun onDataChannel(channel: DataChannel) {
            diagnostics.append("data", "Unexpected remote DataChannel label=${channel.label()}")
            channel.close()
            channel.dispose()
        }

        override fun onIceConnectionReceivingChange(receiving: Boolean) = Unit
        override fun onIceCandidatesRemoved(candidates: Array<out IceCandidate>) = Unit
        override fun onAddStream(stream: MediaStream) = Unit
        override fun onRemoveStream(stream: MediaStream) = Unit
        override fun onRenegotiationNeeded() = Unit
    }

    @Synchronized
    private fun handleRemoteTrack(track: MediaStreamTrack?) {
        if (stopped || track !is VideoTrack) return
        val existing = remoteTrack
        if (existing != null && existing.id() != track.id()) {
            fail("Host 发来了多个视频 Track")
            return
        }
        if (existing != null) return
        remoteTrack = track
        videoTrackReady = true
        diagnostics.append("video", "Remote VideoTrack attached id=${track.id()}")
        observer.onVideoTrack(track)
        maybeStartStream()
    }

    private inner class ControlObserver : DataChannel.Observer {
        override fun onStateChange() {
            val state = control?.state() ?: return
            diagnostics.append("data", "state=$state")
            when (state) {
                DataChannel.State.OPEN -> {
                    controlOpen = true
                    sendSignal(SignalFrame(SignalType.READY))
                    maybeAcknowledge()
                }
                DataChannel.State.CLOSED -> if (!stopped) fail("控制 DataChannel 已关闭")
                else -> Unit
            }
        }

        override fun onMessage(buffer: DataChannel.Buffer) {
            if (!buffer.binary) return fail("DataChannel 收到了非二进制消息")
            val source = buffer.data.duplicate()
            val bytes = ByteArray(source.remaining())
            source.get(bytes)
            handleControlMessage(bytes)
        }

        override fun onBufferedAmountChange(previousAmount: Long) = Unit
    }

    @Synchronized
    private fun handleControlMessage(bytes: ByteArray) {
        if (stopped) return
        try {
            val magic = if (bytes.size >= Int.SIZE_BYTES) {
                ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).int
            } else {
                0
            }
            if (magic == Protocol.CLIPBOARD_MAGIC) {
                val clipboard = ClipboardCodec.decode(bytes)
                diagnostics.append("clipboard", "Received text bytes=${bytes.size - 16} seq=${clipboard.sequence}")
                return
            }
            if (streamHeader != null) throw IllegalArgumentException("重复的 StreamHeader")
            val header = StreamHeaderCodec.decode(bytes)
            streamHeader = header
            diagnostics.append(
                "stream",
                "codec=${header.codec} ${header.width}x${header.height}@${header.fpsNum} bitrate=${header.bitrateBps}",
            )
            observer.onStreamHeader(header)
            if (!sendControl(StreamReadyCodec.encode())) throw IllegalStateException("发送 StreamReady 失败")
            status("等待第一张 ${header.codec} 画面…")
        } catch (error: Throwable) {
            fail("控制消息无效：${error.message}")
        }
    }

    private inner class SignalListener : SignalWebSocket.Listener {
        override fun onRegistered() {
            diagnostics.append("signal", "Invite registered")
            createPeerConnection()
        }

        override fun onBinaryMessage(bytes: ByteArray) {
            try {
                val frames = signalBuffer.push(bytes)
                diagnostics.append("signal", "Binary message bytes=${bytes.size} frames=${frames.size}")
                frames.forEach(::handleSignalFrame)
            } catch (error: Throwable) {
                fail("信令帧无效：${error.message}")
            }
        }

        override fun onFailure(message: String, cause: Throwable?) {
            diagnostics.append("signal", "$message (${cause?.javaClass?.simpleName ?: "no cause"})")
            if (!bootstrapComplete && !stopped) {
                fail(message)
            } else {
                diagnostics.append("signal", "WSS failed after bootstrap; keeping WebRTC session")
            }
        }

        override fun onClosed(reason: String) {
            if (!bootstrapComplete && !stopped) fail("WebRTC 握手完成前 WSS 关闭：$reason")
            else diagnostics.append("signal", "WSS closed after bootstrap")
        }
    }

    private fun handleSignalFrame(frame: SignalFrame) {
        if (stopped) return
        val frameCount = synchronized(this) {
            val count = (receivedSignalCounts[frame.type] ?: 0) + 1
            receivedSignalCounts[frame.type] = count
            count
        }
        diagnostics.append("signal", "Received type=${frame.type} count=$frameCount")
        when (frame.type) {
            SignalType.DESCRIPTION -> {
                if (remoteDescriptionSet) return fail("Host 发来了多个 SDP description")
                val inlineCandidates = sdpCandidateSummaries(frame.value)
                diagnostics.append(
                    "signal",
                    "Remote SDP type=${frame.metadata} inlineICE=${inlineCandidates.size} " +
                        "endOfCandidates=${frame.value.lineSequence().any { it.trim() == "a=end-of-candidates" }}",
                )
                inlineCandidates.forEachIndexed { index, summary ->
                    diagnostics.append("ice", "remote SDP candidate #${index + 1} $summary")
                }
                synchronized(this) {
                    remoteMidToMLineIndex.clear()
                    remoteMidToMLineIndex.putAll(parseMidToMLineIndex(frame.value))
                }
                diagnostics.append("signal", "Remote SDP m-lines=${remoteMidToMLineIndex.size}")
                val description = SessionDescription(
                    SessionDescription.Type.fromCanonicalForm(frame.metadata),
                    frame.value,
                )
                val callback = object : SimpleSdpObserver() {
                    override fun onSetSuccess() {
                        onRemoteDescriptionSet()
                    }

                    override fun onSetFailure(error: String) {
                        remoteSdpObserver = null
                        fail("设置 remote SDP 失败：$error")
                    }
                }
                remoteSdpObserver = callback
                peer?.setRemoteDescription(callback, description)
            }
            SignalType.CANDIDATE -> {
                val mid = frame.metadata.ifBlank { "0" }
                val mLineIndex = synchronized(this) { remoteMidToMLineIndex[mid] ?: 0 }
                val candidate = IceCandidate(mid, mLineIndex, frame.value)
                val candidateCount = synchronized(this) {
                    remoteCandidateCount += 1
                    remoteCandidateCount
                }
                diagnostics.append(
                    "ice",
                    "remote candidate #$candidateCount mid=$mid mline=$mLineIndex " +
                        candidateSummary(frame.value),
                )
                val addNow = synchronized(this) {
                    if (remoteDescriptionSet) true
                    else {
                        pendingCandidates += candidate
                        false
                    }
                }
                if (addNow) addRemoteCandidate(candidate, "trickle")
            }
            SignalType.READY -> {
                remoteReady = true
                maybeAcknowledge()
            }
            SignalType.ACKNOWLEDGED -> {
                remoteAcknowledged = true
                maybeFinishBootstrap()
            }
        }
    }

    @Synchronized
    private fun onRemoteDescriptionSet() {
        if (stopped) return
        remoteSdpObserver = null
        remoteDescriptionSet = true
        val candidates = pendingCandidates.toList()
        pendingCandidates.clear()
        candidates.forEach { addRemoteCandidate(it, "queued trickle") }
        diagnostics.append("signal", "Remote SDP set; pending ICE=${candidates.size}")
    }

    private fun addRemoteCandidate(candidate: IceCandidate, source: String) {
        val accepted = peer?.addIceCandidate(candidate) == true
        diagnostics.append("ice", "$source candidate accepted=$accepted")
        if (!accepted) fail("Host ICE candidate 无法加入 PeerConnection")
    }

    @Synchronized
    private fun maybeAcknowledge() {
        if (stopped || acknowledgementSent || !controlOpen || !gatheringComplete || !remoteReady) return
        acknowledgementSent = true
        sendSignal(SignalFrame(SignalType.ACKNOWLEDGED))
        maybeFinishBootstrap()
    }

    @Synchronized
    private fun maybeFinishBootstrap() {
        if (stopped || bootstrapComplete || !controlOpen || !acknowledgementSent ||
            !remoteAcknowledged
        ) return
        bootstrapComplete = true
        status("WebRTC 已连接，正在请求视频…")
        diagnostics.append("signal", "Bootstrap complete")
        maybeStartStream()
    }

    @Synchronized
    private fun maybeStartStream() {
        if (stopped || streamRequested || !bootstrapComplete || !videoTrackReady) return
        streamRequested = true
        if (!sendControl(ClientConfigCodec.encode(clientConfig))) {
            fail("发送 ClientConfig 失败")
        }
    }

    private fun sendSignal(frame: SignalFrame) {
        if (stopped) return
        val sent = signalSocket.send(SignalFrameCodec.encode(frame))
        diagnostics.append("signal", "Sent type=${frame.type} accepted=$sent")
        if (!sent) {
            fail("WSS 信令连接未打开")
        }
    }

    private fun sendControl(bytes: ByteArray): Boolean {
        val channel = control ?: return false
        return channel.state() == DataChannel.State.OPEN &&
            channel.send(DataChannel.Buffer(ByteBuffer.wrap(bytes), true))
    }

    @Synchronized
    private fun flushPendingMove(force: Boolean = false): Boolean {
        inputFrameScheduled = false
        val event = pendingMove ?: return true
        val channel = control
        // Do not add replaceable input to an already congested reliable channel.
        if (!force && channel != null && channel.bufferedAmount() > MAX_BUFFERED_INPUT_BYTES) {
            schedulePendingMove()
            return true
        }
        pendingMove = null
        return sendInputNow(event)
    }

    private fun schedulePendingMove() {
        if (inputFrameScheduled || stopped) return
        inputFrameScheduled = true
        inputChoreographer.postFrameCallback(inputFrameCallback)
    }

    private fun sendInputNow(event: RemoteInputEvent): Boolean {
        if (stopped || !bootstrapComplete || streamHeader == null) return false
        return sendControl(InputEventCodec.encode(event.copy(sequence = inputSequence++)))
    }

    private fun startStats() {
        if (statsTask != null) return
        statsTask = statsExecutor.scheduleAtFixedRate(
            { if (!stopped) peer?.getStats(::reportStats) },
            0,
            1,
            TimeUnit.SECONDS,
        )
    }

    private fun reportStats(report: RTCStatsReport) {
        if (stopped || !performanceStatsEnabled) return
        reportIceStats(report)
        val inbound = report.statsMap.values.firstOrNull { stat ->
            stat.type == "inbound-rtp" &&
                (stat.members["kind"] == "video" || stat.members["mediaType"] == "video")
        } ?: return
        val members = inbound.members
        val codecId = members["codecId"] as? String
        val codec = codecId?.let { report.statsMap[it]?.members?.get("mimeType") as? String } ?: "unknown"
        val summary = buildString {
            append("codec=$codec")
            append(" decoder=${members["decoderImplementation"] ?: "unknown"}")
            append(" ${number(members["frameWidth"])}x${number(members["frameHeight"])}")
            append(" fps=${decimal(members["framesPerSecond"])}")
            append(" bytes=${number(members["bytesReceived"])}")
            append(" packets=${number(members["packetsReceived"])}")
            append(" lost=${number(members["packetsLost"])}")
            append(" jitter=${decimal(members["jitter"])}s")
            append(" decoded=${number(members["framesDecoded"])}")
            append(" dropped=${number(members["framesDropped"])}")
        }
        diagnostics.append("stats", summary)
        performanceStatsCalculator.update(
            timestampNanos = SystemClock.elapsedRealtimeNanos(),
            bytesReceived = number(members["bytesReceived"]),
            framesDecoded = number(members["framesDecoded"] ?: members["framesReceived"]),
            packetsLost = number(members["packetsLost"]),
        )?.let(observer::onPerformanceStats)
    }

    private fun reportIceStats(report: RTCStatsReport) {
        val pairs = report.statsMap.values.filter { it.type == "candidate-pair" }
        if (pairs.isEmpty()) return
        val states = pairs.groupingBy { it.members["state"]?.toString() ?: "unknown" }
            .eachCount()
            .toSortedMap()
            .entries
            .joinToString(",") { "${it.key}:${it.value}" }
        val summary = buildString {
            append("pairs=${pairs.size} states=$states")
            append(" nominated=${pairs.count { it.members["nominated"] == true }}")
            append(" sent=${pairs.sumOf { number(it.members["requestsSent"]) }}")
            append(" recv=${pairs.sumOf { number(it.members["requestsReceived"]) }}")
            append(" responsesOut=${pairs.sumOf { number(it.members["responsesSent"]) }}")
            append(" responses=${pairs.sumOf { number(it.members["responsesReceived"]) }}")
            append(" bytesOut=${pairs.sumOf { number(it.members["bytesSent"]) }}")
            append(" bytesIn=${pairs.sumOf { number(it.members["bytesReceived"]) }}")
        }
        if (summary != lastIceStatsSummary) {
            lastIceStatsSummary = summary
            diagnostics.append("ice-stats", summary)
        }
    }

    private fun number(value: Any?): Long = (value as? Number)?.toLong() ?: 0L
    private fun decimal(value: Any?): String = "%.3f".format((value as? Number)?.toDouble() ?: 0.0)

    private fun candidateSummary(sdp: String): String {
        val fields = sdp.trim().split(Regex("\\s+"))
        val protocol = fields.getOrNull(2)?.lowercase() ?: "unknown"
        val address = fields.getOrNull(4).orEmpty()
        val addressFamily = when {
            address.endsWith(".local", true) -> "mdns"
            ':' in address -> "ipv6"
            '.' in address -> "ipv4"
            else -> "unknown"
        }
        val typeIndex = fields.indexOf("typ")
        val type = if (typeIndex >= 0) fields.getOrNull(typeIndex + 1) ?: "unknown" else "unknown"
        return "type=$type protocol=$protocol family=$addressFamily"
    }

    private fun sdpCandidateSummaries(sdp: String): List<String> = sdp
        .lineSequence()
        .map(String::trim)
        .filter { it.startsWith("a=candidate:") }
        .map { candidateSummary(it.removePrefix("a=")) }
        .toList()

    private fun parseMidToMLineIndex(sdp: String): Map<String, Int> {
        val result = mutableMapOf<String, Int>()
        var mLineIndex = -1
        sdp.lineSequence().forEach { rawLine ->
            val line = rawLine.trim()
            if (line.startsWith("m=")) mLineIndex += 1
            else if (line.startsWith("a=mid:") && mLineIndex >= 0) {
                result[line.removePrefix("a=mid:")] = mLineIndex
            }
        }
        return result
    }

    private fun status(message: String) {
        diagnostics.append("status", message)
        observer.onStatus(message)
    }

    @Synchronized
    private fun fail(message: String) {
        if (stopped) return
        diagnostics.append("error", message)
        close("连接已停止")
        observer.onError(message)
    }

    private open inner class SimpleSdpObserver : SdpObserver {
        override fun onCreateSuccess(description: SessionDescription) = Unit
        override fun onSetSuccess() = Unit
        override fun onCreateFailure(error: String) = fail(error)
        override fun onSetFailure(error: String) = fail(error)
    }

    private companion object {
        const val MAX_BUFFERED_INPUT_BYTES = 8L * 1024L
    }
}
