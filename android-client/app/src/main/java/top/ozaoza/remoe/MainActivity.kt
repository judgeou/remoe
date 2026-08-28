package top.ozaoza.remoe

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer
import org.webrtc.VideoTrack
import top.ozaoza.remoe.protocol.ClientConfig
import top.ozaoza.remoe.binding.ActiveBinding
import top.ozaoza.remoe.binding.AndroidBindingClient
import top.ozaoza.remoe.binding.BindInviteParser
import top.ozaoza.remoe.binding.BindingState
import top.ozaoza.remoe.binding.QrScannerActivity
import top.ozaoza.remoe.rtc.RtcCodecProbe
import top.ozaoza.remoe.rtc.RtcSession
import top.ozaoza.remoe.signaling.InviteParser
import java.util.concurrent.Executors

class MainActivity : Activity(), RtcSession.Observer, RendererCommon.RendererEvents {
    private val probeExecutor = Executors.newSingleThreadExecutor()
    private lateinit var app: RemoeApplication
    private lateinit var renderer: SurfaceViewRenderer
    private lateinit var inviteInput: EditText
    private lateinit var fpsInput: EditText
    private lateinit var bitrateInput: EditText
    private lateinit var scaleInput: EditText
    private lateinit var statusView: TextView
    private lateinit var diagnosticsView: TextView
    private lateinit var connectButton: Button
    private lateinit var disconnectButton: Button
    private lateinit var codecProbeButton: Button
    private lateinit var scanBindButton: Button
    private lateinit var bindingStatusView: TextView
    private lateinit var bindingClient: AndroidBindingClient
    private val bindingHandler = Handler(Looper.getMainLooper())
    private var activeBinding: ActiveBinding? = null
    private var bindingForeground = false
    private var session: RtcSession? = null
    private var remoteTrack: VideoTrack? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        app = application as RemoeApplication
        bindingClient = AndroidBindingClient(app.httpClient)

        renderer = SurfaceViewRenderer(this).apply {
            init(app.rtcRuntime.eglBase.eglBaseContext, this@MainActivity)
            setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            setEnableHardwareScaler(true)
            setMirror(false)
            setBackgroundColor(Color.BLACK)
        }

        val root = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(renderer, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            addView(createControlPanel(), FrameLayout.LayoutParams(
                dp(410),
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.START,
            ))
        }
        setContentView(root)
        showStatus("粘贴 Host invite 后连接")
    }

    private fun createControlPanel(): ScrollView {
        inviteInput = editText(getString(R.string.invite_hint), InputType.TYPE_CLASS_TEXT).apply {
            isSingleLine = true
        }
        fpsInput = editText("FPS", InputType.TYPE_CLASS_NUMBER).apply { setText("60") }
        bitrateInput = editText("Mbps", InputType.TYPE_CLASS_NUMBER).apply { setText("20") }
        scaleInput = editText("Scale %", InputType.TYPE_CLASS_NUMBER).apply { setText("100") }
        statusView = textView(15f, Color.WHITE).apply { typeface = Typeface.DEFAULT_BOLD }
        diagnosticsView = textView(11f, Color.rgb(205, 214, 228)).apply {
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        }
        connectButton = Button(this).apply {
            text = getString(R.string.connect)
            setOnClickListener { connect() }
        }
        disconnectButton = Button(this).apply {
            text = getString(R.string.disconnect)
            isEnabled = false
            setOnClickListener { disconnect("用户断开") }
        }
        codecProbeButton = Button(this).apply {
            text = getString(R.string.run_codec_probe)
            setOnClickListener { runCodecProbe() }
        }
        scanBindButton = Button(this).apply {
            text = "扫描账号绑定二维码"
            setOnClickListener {
                startActivityForResult(
                    Intent(this@MainActivity, QrScannerActivity::class.java),
                    REQUEST_BIND_QR,
                )
            }
        }
        bindingStatusView = textView(14f, Color.rgb(205, 214, 228))

        val settings = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(fpsInput, LinearLayout.LayoutParams(0, dp(52), 1f))
            addView(bitrateInput, LinearLayout.LayoutParams(0, dp(52), 1f))
            addView(scaleInput, LinearLayout.LayoutParams(0, dp(52), 1f))
        }
        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(connectButton, LinearLayout.LayoutParams(0, dp(52), 1f))
            addView(disconnectButton, LinearLayout.LayoutParams(0, dp(52), 1f))
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(14), dp(16), dp(24))
            addView(textView(22f, Color.WHITE).apply {
                text = getString(R.string.stream_probe_title)
                typeface = Typeface.DEFAULT_BOLD
            })
            addView(inviteInput, matchWrap(top = 10))
            addView(settings, matchWrap(top = 6))
            addView(actions, matchWrap(top = 6))
            addView(codecProbeButton, matchWrap(top = 6))
            addView(textView(18f, Color.WHITE).apply {
                text = "阶段 D · 账号绑定"
                typeface = Typeface.DEFAULT_BOLD
            }, matchWrap(top = 18))
            addView(scanBindButton, matchWrap(top = 6))
            addView(bindingStatusView, matchWrap(top = 8))
            addView(statusView, matchWrap(top = 12))
            addView(diagnosticsView, matchWrap(top = 10))
        }
        return ScrollView(this).apply {
            setBackgroundColor(Color.argb(225, 18, 21, 27))
            addView(content)
        }
    }

    @Deprecated("Activity result compatibility for the current programmatic Activity UI")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_BIND_QR || resultCode != RESULT_OK) return
        val raw = data?.getStringExtra(QrScannerActivity.EXTRA_RESULT).orEmpty()
        val invite = try {
            BindInviteParser.parse(raw)
        } catch (error: IllegalArgumentException) {
            bindingStatusView.setTextColor(Color.rgb(255, 130, 130))
            bindingStatusView.text = error.message
            return
        }
        scanBindButton.isEnabled = false
        bindingStatusView.setTextColor(Color.rgb(205, 214, 228))
        bindingStatusView.text = "正在安全认领绑定…"
        bindingClient.claim(invite) { result ->
            postUi {
                result.onSuccess { (active, state) ->
                    activeBinding = active
                    renderBindingState(state)
                    scheduleBindingPoll()
                }.onFailure { error ->
                    scanBindButton.isEnabled = true
                    bindingStatusView.setTextColor(Color.rgb(255, 130, 130))
                    bindingStatusView.text = "绑定失败：${error.message}"
                }
            }
        }
    }

    private fun scheduleBindingPoll(delayMs: Long = 1_500) {
        bindingHandler.removeCallbacks(bindingPoll)
        if (activeBinding != null && bindingForeground && !isFinishing) {
            bindingHandler.postDelayed(bindingPoll, delayMs)
        }
    }

    private val bindingPoll = object : Runnable {
        override fun run() {
            val binding = activeBinding ?: return
            bindingClient.status(binding) { result ->
                postUi {
                    result.onSuccess(::renderBindingState).onFailure { error ->
                        bindingStatusView.setTextColor(Color.rgb(255, 130, 130))
                        bindingStatusView.text = "检查绑定状态失败：${error.message}"
                    }
                    if (activeBinding != null) scheduleBindingPoll(2_000)
                }
            }
        }
    }

    private fun renderBindingState(state: BindingState) {
        bindingStatusView.setTextColor(Color.rgb(205, 214, 228))
        when (state.status) {
            "claimed" -> bindingStatusView.text =
                "请在网页核对并批准：\n${state.comparisonCode ?: "核对码不可用"}"
            "approved" -> {
                bindingStatusView.setTextColor(Color.rgb(90, 225, 175))
                bindingStatusView.text = "网页已批准。阶段 E 将在此创建本机 passkey。"
                finishBindingPolling()
            }
            "rejected" -> {
                bindingStatusView.text = "网页已拒绝本次绑定。"
                finishBindingPolling()
            }
            "expired" -> {
                bindingStatusView.text = "绑定已过期，请重新扫描。"
                finishBindingPolling()
            }
            else -> bindingStatusView.text = "等待网页确认…"
        }
    }

    private fun finishBindingPolling() {
        activeBinding = null
        bindingHandler.removeCallbacks(bindingPoll)
        scanBindButton.isEnabled = true
    }

    private fun connect() {
        if (session != null) return
        val invite = try {
            InviteParser.parse(inviteInput.text.toString())
        } catch (error: IllegalArgumentException) {
            return showStatus(error.message ?: "邀请无效", true)
        }
        val fps = fpsInput.text.toString().toIntOrNull()
        val bitrateMbps = bitrateInput.text.toString().toIntOrNull()
        val scale = scaleInput.text.toString().toIntOrNull()
        if (fps !in 1..240 || bitrateMbps !in 1..1000 || scale !in 10..100) {
            return showStatus("FPS/码率/缩放参数无效", true)
        }
        val config = ClientConfig(
            fpsNum = fps!!,
            bitrateBps = bitrateMbps!! * 1_000_000,
            scalePercent = scale!!,
        )
        diagnosticsView.text = "log: ${app.diagnosticLog.path()}"
        connectButton.isEnabled = false
        disconnectButton.isEnabled = true
        codecProbeButton.isEnabled = false
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        session = RtcSession(
            runtime = app.rtcRuntime,
            httpClient = app.httpClient,
            invite = invite,
            clientConfig = config,
            diagnostics = app.diagnosticLog,
            observer = this,
        ).also { it.connect() }
    }

    private fun disconnect(reason: String) {
        detachRemoteTrack()
        session?.close(reason)
        session = null
        connectButton.isEnabled = true
        disconnectButton.isEnabled = false
        codecProbeButton.isEnabled = true
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        renderer.clearImage()
    }

    private fun runCodecProbe() {
        if (session != null) return
        codecProbeButton.isEnabled = false
        diagnosticsView.text = getString(R.string.codec_probe_running)
        probeExecutor.execute {
            val report = RtcCodecProbe(app.rtcRuntime).run()
            runOnUiThread {
                if (!isDestroyed) {
                    diagnosticsView.text = report
                    codecProbeButton.isEnabled = true
                }
            }
        }
    }

    override fun onStatus(message: String) = postUi { showStatus(message) }

    override fun onVideoTrack(track: VideoTrack) = postUi {
        if (session == null) return@postUi
        detachRemoteTrack()
        remoteTrack = track
        track.addSink(renderer)
        showStatus("VideoTrack 已连接，等待 StreamHeader…")
    }

    override fun onStreamHeader(header: top.ozaoza.remoe.protocol.StreamHeader) = postUi {
        showStatus("${header.codec} ${header.width}×${header.height} @ ${header.fpsNum} FPS")
    }

    override fun onDiagnostics(summary: String) = postUi {
        diagnosticsView.text = "$summary\n\n${app.diagnosticLog.snapshot()}"
    }

    override fun onError(message: String) = postUi {
        detachRemoteTrack()
        session = null
        connectButton.isEnabled = true
        disconnectButton.isEnabled = false
        codecProbeButton.isEnabled = true
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        showStatus(message, true)
    }

    override fun onFirstFrameRendered() = postUi { showStatus("第一帧已渲染") }

    override fun onFrameResolutionChanged(width: Int, height: Int, rotation: Int) = postUi {
        app.diagnosticLog.append("renderer", "resolution=${width}x$height rotation=$rotation")
    }

    override fun onStop() {
        bindingForeground = false
        bindingHandler.removeCallbacks(bindingPoll)
        if (session != null) disconnect("进入后台，连接已断开")
        super.onStop()
    }

    override fun onStart() {
        super.onStart()
        bindingForeground = true
        if (activeBinding != null) scheduleBindingPoll(0)
    }

    override fun onDestroy() {
        detachRemoteTrack()
        renderer.release()
        probeExecutor.shutdownNow()
        bindingHandler.removeCallbacks(bindingPoll)
        super.onDestroy()
    }

    private fun detachRemoteTrack() {
        remoteTrack?.removeSink(renderer)
        remoteTrack = null
    }

    private fun showStatus(message: String, error: Boolean = false) {
        statusView.setTextColor(if (error) Color.rgb(255, 130, 130) else Color.WHITE)
        statusView.text = message
    }

    private fun postUi(action: () -> Unit) {
        runOnUiThread { if (!isDestroyed) action() }
    }

    private fun editText(hintText: String, type: Int): EditText = EditText(this).apply {
        hint = hintText
        inputType = type
        setTextColor(Color.WHITE)
        setHintTextColor(Color.rgb(145, 154, 170))
        setBackgroundColor(Color.rgb(38, 44, 56))
        setPadding(dp(10), 0, dp(10), 0)
    }

    private fun textView(size: Float, color: Int): TextView = TextView(this).apply {
        textSize = size
        setTextColor(color)
    }

    private fun matchWrap(top: Int = 0): LinearLayout.LayoutParams = LinearLayout.LayoutParams(
        LinearLayout.LayoutParams.MATCH_PARENT,
        LinearLayout.LayoutParams.WRAP_CONTENT,
    ).apply { topMargin = dp(top) }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        private const val REQUEST_BIND_QR = 4101
    }
}
