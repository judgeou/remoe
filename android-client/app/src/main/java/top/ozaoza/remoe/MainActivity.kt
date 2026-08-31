package top.ozaoza.remoe

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.webrtc.RendererCommon
import org.webrtc.VideoSink
import org.webrtc.VideoTrack
import top.ozaoza.remoe.protocol.ClientConfig
import top.ozaoza.remoe.auth.AndroidDeviceProtocol
import top.ozaoza.remoe.auth.DeviceIdentityStore
import top.ozaoza.remoe.auth.NativeSessionStore
import top.ozaoza.remoe.binding.ActiveBinding
import top.ozaoza.remoe.binding.AndroidBindingClient
import top.ozaoza.remoe.binding.BindInviteParser
import top.ozaoza.remoe.binding.BindingState
import top.ozaoza.remoe.binding.QrScannerActivity
import top.ozaoza.remoe.input.RemoteTouchController
import top.ozaoza.remoe.rtc.RtcCodecProbe
import top.ozaoza.remoe.rtc.RtcSession
import top.ozaoza.remoe.rtc.TextureViewVideoRenderer
import top.ozaoza.remoe.signaling.InviteParser
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : ComponentActivity(), RtcSession.Observer, RendererCommon.RendererEvents {
    private val probeExecutor = Executors.newSingleThreadExecutor()
    private lateinit var app: RemoeApplication
    private lateinit var renderer: TextureViewVideoRenderer
    private lateinit var touchController: RemoteTouchController
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
    private lateinit var deviceLoginButton: Button
    private lateinit var bindingStatusView: TextView
    private lateinit var hostListView: LinearLayout
    private lateinit var controlPanel: ScrollView
    private lateinit var remoteStopButton: Button
    private lateinit var developerPanel: LinearLayout
    private lateinit var bindingClient: AndroidBindingClient
    private lateinit var deviceIdentityStore: DeviceIdentityStore
    private lateinit var nativeSessionStore: NativeSessionStore
    private val bindingHandler = Handler(Looper.getMainLooper())
    private var activeBinding: ActiveBinding? = null
    private var bindingForeground = false
    private var session: RtcSession? = null
    private var remoteTrack: VideoTrack? = null
    private val firstSinkFrame = AtomicBoolean(false)
    private val remoteVideoSink = VideoSink { frame ->
        if (firstSinkFrame.compareAndSet(false, true)) {
            app.diagnosticLog.append(
                "renderer",
                "first sink frame=${frame.buffer.width}x${frame.buffer.height} rotation=${frame.rotation}",
            )
        }
        renderer.onFrame(frame)
    }
    private val qrScannerLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            handleBindingQr(result.data?.getStringExtra(QrScannerActivity.EXTRA_RESULT).orEmpty())
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        app = application as RemoeApplication
        bindingClient = AndroidBindingClient(app.httpClient)
        deviceIdentityStore = DeviceIdentityStore(this)
        nativeSessionStore = NativeSessionStore(this)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT

        renderer = TextureViewVideoRenderer(this).apply {
            init(app.rtcRuntime.eglBase.eglBaseContext, this@MainActivity)
            setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            setMirror(false)
        }
        val remoteTouchLayer = View(this)
        touchController = RemoteTouchController(remoteTouchLayer, renderer) { input ->
            session?.sendInput(input) == true
        }

        controlPanel = createControlPanel()
        remoteStopButton = secondaryButton("断开").apply {
            visibility = View.GONE
            setOnClickListener { disconnect("用户断开") }
        }

        val root = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(renderer, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER,
            ))
            addView(remoteTouchLayer, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            addView(controlPanel, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            addView(remoteStopButton, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).apply { setMargins(dp(12), dp(12), dp(12), dp(12)) })
        }
        setContentView(root)
        showStatus("登录后选择一台在线电脑")
        restoreNativeSession()
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
        connectButton = primaryButton(getString(R.string.connect)).apply { setOnClickListener { connect() } }
        disconnectButton = secondaryButton(getString(R.string.disconnect)).apply {
            isEnabled = false
            setOnClickListener { disconnect("用户断开") }
        }
        codecProbeButton = secondaryButton(getString(R.string.run_codec_probe)).apply {
            setOnClickListener { runCodecProbe() }
        }
        scanBindButton = primaryButton("扫描网页二维码").apply {
            setOnClickListener {
                qrScannerLauncher.launch(Intent(this@MainActivity, QrScannerActivity::class.java))
            }
        }
        deviceLoginButton = secondaryButton("使用本机密钥登录").apply {
            setOnClickListener { loginWithDeviceKey() }
        }
        bindingStatusView = textView(14f, Color.rgb(205, 214, 228))
        hostListView = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val accountActions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(secondaryButton("刷新").apply {
                setOnClickListener { loadHosts() }
            }, LinearLayout.LayoutParams(0, dp(52), 1f))
            addView(secondaryButton("退出账号").apply {
                setOnClickListener { logoutNativeSession() }
            }, LinearLayout.LayoutParams(0, dp(52), 1f).apply { marginStart = dp(8) })
        }

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
        developerPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(inviteInput, matchWrap(top = 10))
            addView(settings, matchWrap(top = 6))
            addView(actions, matchWrap(top = 6))
            addView(codecProbeButton, matchWrap(top = 6))
            addView(diagnosticsView, matchWrap(top = 10))
            visibility = View.GONE
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(22), dp(28), dp(22), dp(36))
            addView(textView(34f, Color.WHITE).apply {
                text = "remoe"
                typeface = Typeface.DEFAULT_BOLD
            })
            addView(textView(14f, Color.rgb(145, 154, 170)).apply {
                text = "从手机安全连接你的电脑"
            }, matchWrap(top = 4))
            addView(textView(20f, Color.WHITE).apply {
                text = "账号"
                typeface = Typeface.DEFAULT_BOLD
            }, matchWrap(top = 30))
            addView(scanBindButton, matchWrap(top = 12))
            addView(deviceLoginButton, matchWrap(top = 8))
            addView(bindingStatusView, matchWrap(top = 12))
            addView(textView(20f, Color.WHITE).apply {
                text = "我的电脑"
                typeface = Typeface.DEFAULT_BOLD
            }, matchWrap(top = 30))
            addView(accountActions, matchWrap(top = 8))
            addView(hostListView, matchWrap(top = 12))
            addView(statusView, matchWrap(top = 20))
            addView(secondaryButton("开发工具").apply {
                setOnClickListener {
                    developerPanel.visibility =
                        if (developerPanel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                }
            }, matchWrap(top = 28))
            addView(developerPanel)
        }
        return ScrollView(this).apply {
            setBackgroundColor(Color.rgb(8, 11, 16))
            isFillViewport = true
            addView(content)
        }
    }

    private fun handleBindingQr(raw: String) {
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
                beginDeviceRegistration()
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

    private fun beginDeviceRegistration() {
        val binding = activeBinding ?: return
        activeBinding = null
        bindingHandler.removeCallbacks(bindingPoll)
        bindingStatusView.setTextColor(Color.rgb(205, 214, 228))
        bindingStatusView.text = "网页已批准，正在创建设备密钥…"
        probeExecutor.execute {
            try {
                val identity = deviceIdentityStore.getOrCreate()
                bindingClient.registrationOptions(binding) { result ->
                    result.onSuccess { challenge ->
                        try {
                            val message = AndroidDeviceProtocol.registrationMessage(
                                challenge.ceremonyId,
                                challenge.challenge,
                                binding.bindingId,
                                identity.deviceId,
                                identity.publicKey,
                            )
                            val signature = deviceIdentityStore.sign(message)
                            postUi { bindingStatusView.text = "正在验证设备密钥…" }
                            bindingClient.verifyRegistration(
                                binding,
                                challenge.ceremonyId,
                                identity.deviceId,
                                identity.publicKey,
                                signature,
                            ) { verification ->
                                postUi {
                                    verification.onSuccess { tokens ->
                                        nativeSessionStore.save(tokens)
                                        bindingStatusView.setTextColor(Color.rgb(90, 225, 175))
                                        bindingStatusView.text = "本机已安全绑定到账号。"
                                        loadHosts()
                                    }.onFailure(::showBindingFailure)
                                    scanBindButton.isEnabled = true
                                }
                            }
                        } catch (error: Exception) {
                            postUi {
                                showBindingFailure(error)
                                scanBindButton.isEnabled = true
                            }
                        }
                    }.onFailure { error ->
                        postUi {
                            showBindingFailure(error)
                            scanBindButton.isEnabled = true
                        }
                    }
                }
            } catch (error: Exception) {
                postUi {
                    showBindingFailure(error)
                    scanBindButton.isEnabled = true
                }
            }
        }
    }

    private fun loginWithDeviceKey() {
        deviceLoginButton.isEnabled = false
        bindingStatusView.setTextColor(Color.rgb(205, 214, 228))
        bindingStatusView.text = "正在验证本机身份…"
        probeExecutor.execute {
            val identity = deviceIdentityStore.current()
            if (identity == null) {
                postUi {
                    showBindingFailure(IllegalStateException("本机尚未绑定，请先扫描网页二维码"))
                    deviceLoginButton.isEnabled = true
                }
                return@execute
            }
            bindingClient.loginOptions(identity.deviceId) { result ->
                result.onSuccess { challenge ->
                    try {
                        val signature = deviceIdentityStore.sign(
                            AndroidDeviceProtocol.loginMessage(
                                challenge.ceremonyId, challenge.challenge, identity.deviceId,
                            ),
                        )
                        bindingClient.verifyLogin(
                            challenge.ceremonyId, identity.deviceId, signature,
                        ) { verification ->
                            postUi {
                                verification.onSuccess { tokens ->
                                    nativeSessionStore.save(tokens)
                                    bindingStatusView.setTextColor(Color.rgb(90, 225, 175))
                                    bindingStatusView.text = "本机密钥登录成功。"
                                    loadHosts()
                                }.onFailure(::showBindingFailure)
                                deviceLoginButton.isEnabled = true
                            }
                        }
                    } catch (error: Exception) {
                        postUi {
                            showBindingFailure(error)
                            deviceLoginButton.isEnabled = true
                        }
                    }
                }.onFailure { error ->
                    postUi {
                        showBindingFailure(error)
                        deviceLoginButton.isEnabled = true
                    }
                }
            }
        }
    }

    private fun showBindingFailure(error: Throwable) {
        bindingStatusView.setTextColor(Color.rgb(255, 130, 130))
        bindingStatusView.text = "账号验证失败：${error.message ?: error.javaClass.simpleName}"
    }

    private fun restoreNativeSession() {
        val refreshToken = nativeSessionStore.refreshToken() ?: return
        deviceLoginButton.isEnabled = false
        bindingStatusView.text = "正在恢复 Android 登录…"
        bindingClient.refresh(refreshToken) { result ->
            postUi {
                result.onSuccess { grant ->
                    nativeSessionStore.updateAccess(grant.accessToken, grant.expiresIn)
                    bindingStatusView.setTextColor(Color.rgb(90, 225, 175))
                    bindingStatusView.text = "Android 登录已恢复。"
                    loadHosts()
                }.onFailure {
                    nativeSessionStore.clear()
                    showBindingFailure(it)
                }
                deviceLoginButton.isEnabled = true
            }
        }
    }

    private fun withAccess(action: (String) -> Unit) {
        val current = nativeSessionStore.accessToken
        if (current != null && nativeSessionStore.accessExpiresAt > System.currentTimeMillis() + 30_000) {
            action(current)
            return
        }
        val refreshToken = nativeSessionStore.refreshToken()
        if (refreshToken == null) {
            showBindingFailure(IllegalStateException("请先使用本机密钥登录"))
            return
        }
        bindingClient.refresh(refreshToken) { result ->
            postUi {
                result.onSuccess { grant ->
                    nativeSessionStore.updateAccess(grant.accessToken, grant.expiresIn)
                    action(grant.accessToken)
                }.onFailure {
                    nativeSessionStore.clear()
                    showBindingFailure(it)
                }
            }
        }
    }

    private fun loadHosts() = withAccess { accessToken ->
        hostListView.removeAllViews()
        hostListView.addView(textView(13f, Color.rgb(145, 154, 170)).apply {
            text = "正在载入电脑…"
        })
        bindingClient.hosts(accessToken) { result ->
            postUi {
                hostListView.removeAllViews()
                result.onSuccess { hosts ->
                    if (hosts.isEmpty()) {
                        hostListView.addView(textView(13f, Color.rgb(145, 154, 170)).apply {
                            text = "账号下还没有 Host。"
                        })
                    }
                    hosts.forEach { host ->
                        hostListView.addView(secondaryButton(
                            "${if (host.online) "●" else "○"} ${host.name}",
                        ).apply {
                            isEnabled = host.online && session == null
                            setOnClickListener { connectManagedHost(host.id) }
                        }, matchWrap(top = 4))
                    }
                }.onFailure(::showBindingFailure)
            }
        }
    }

    private fun connectManagedHost(hostId: String) = withAccess { accessToken ->
        bindingStatusView.text = "正在请求 Host 连接…"
        bindingClient.connectHost(hostId, accessToken) { result ->
            postUi {
                result.onSuccess { managedInvite ->
                    inviteInput.setText(managedInvite)
                    connect()
                }.onFailure(::showBindingFailure)
            }
        }
    }

    private fun logoutNativeSession() {
        val refreshToken = nativeSessionStore.refreshToken()
        if (refreshToken == null) {
            nativeSessionStore.clear()
            hostListView.removeAllViews()
            bindingStatusView.text = "当前没有 Android 登录。"
            return
        }
        bindingClient.logout(refreshToken) { result ->
            postUi {
                nativeSessionStore.clear()
                hostListView.removeAllViews()
                if (result.isSuccess) {
                    bindingStatusView.text = "已退出 Android 账号。"
                } else {
                    bindingStatusView.text = "本地登录已清除；服务端撤销将在会话过期后生效。"
                }
            }
        }
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
        enterRemoteMode()
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
        touchController.cancel()
        detachRemoteTrack()
        session?.close(reason)
        session = null
        connectButton.isEnabled = true
        disconnectButton.isEnabled = false
        codecProbeButton.isEnabled = true
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        renderer.clearImage()
        leaveRemoteMode()
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
        firstSinkFrame.set(false)
        track.addSink(remoteVideoSink)
        showStatus("VideoTrack 已连接，等待 StreamHeader…")
    }

    override fun onStreamHeader(header: top.ozaoza.remoe.protocol.StreamHeader) = postUi {
        showStatus("${header.codec} ${header.width}×${header.height} @ ${header.fpsNum} FPS")
    }

    override fun onDiagnostics(summary: String) = postUi {
        diagnosticsView.text = "$summary\n\n${app.diagnosticLog.snapshot()}"
    }

    override fun onError(message: String) = postUi {
        touchController.cancel()
        detachRemoteTrack()
        session = null
        connectButton.isEnabled = true
        disconnectButton.isEnabled = false
        codecProbeButton.isEnabled = true
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        leaveRemoteMode()
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
        touchController.dispose()
        detachRemoteTrack()
        renderer.release()
        probeExecutor.shutdownNow()
        bindingHandler.removeCallbacks(bindingPoll)
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus && ::touchController.isInitialized) touchController.cancel()
    }

    private fun detachRemoteTrack() {
        remoteTrack?.removeSink(remoteVideoSink)
        remoteTrack = null
        firstSinkFrame.set(false)
    }

    private fun showStatus(message: String, error: Boolean = false) {
        statusView.setTextColor(if (error) Color.rgb(255, 130, 130) else Color.WHITE)
        statusView.text = message
    }

    private fun postUi(action: () -> Unit) {
        runOnUiThread { if (!isDestroyed) action() }
    }

    private fun enterRemoteMode() {
        touchController.resetViewport()
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        controlPanel.visibility = View.GONE
        remoteStopButton.visibility = View.VISIBLE
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    private fun leaveRemoteMode() {
        touchController.resetViewport()
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        remoteStopButton.visibility = View.GONE
        controlPanel.visibility = View.VISIBLE
        WindowCompat.getInsetsController(window, window.decorView)
            .show(WindowInsetsCompat.Type.systemBars())
        WindowCompat.setDecorFitsSystemWindows(window, true)
    }

    private fun primaryButton(label: String): Button = Button(this).apply {
        text = label
        textSize = 15f
        isAllCaps = false
        setTextColor(Color.rgb(6, 16, 13))
        backgroundTintList = ColorStateList.valueOf(Color.rgb(73, 220, 175))
        minHeight = dp(52)
    }

    private fun secondaryButton(label: String): Button = Button(this).apply {
        text = label
        textSize = 15f
        isAllCaps = false
        setTextColor(Color.rgb(225, 233, 240))
        backgroundTintList = ColorStateList.valueOf(Color.rgb(37, 46, 57))
        minHeight = dp(52)
    }

    private fun editText(hintText: String, type: Int): EditText = EditText(this).apply {
        hint = hintText
        inputType = type
        setTextColor(Color.WHITE)
        setHintTextColor(Color.rgb(145, 154, 170))
        setBackgroundColor(Color.rgb(38, 44, 56))
        backgroundTintList = ColorStateList.valueOf(Color.rgb(73, 220, 175))
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

}
