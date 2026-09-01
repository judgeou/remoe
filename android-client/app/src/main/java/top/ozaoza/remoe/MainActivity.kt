package top.ozaoza.remoe

import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Build
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
import top.ozaoza.remoe.auth.AndroidDeviceProtocol
import top.ozaoza.remoe.auth.DeviceIdentityStore
import top.ozaoza.remoe.auth.NativeSessionStore
import top.ozaoza.remoe.binding.ActiveBinding
import top.ozaoza.remoe.binding.AndroidBindingClient
import top.ozaoza.remoe.binding.BindInviteParser
import top.ozaoza.remoe.binding.BindingState
import top.ozaoza.remoe.binding.QrScannerActivity
import top.ozaoza.remoe.input.RemoteCursorView
import top.ozaoza.remoe.input.RemoteTouchController
import top.ozaoza.remoe.rtc.RtcCodecProbe
import top.ozaoza.remoe.rtc.RtcPerformanceStats
import top.ozaoza.remoe.rtc.RtcSession
import top.ozaoza.remoe.rtc.TextureViewVideoRenderer
import top.ozaoza.remoe.signaling.InviteParser
import top.ozaoza.remoe.protocol.VideoRateControl
import top.ozaoza.remoe.settings.VideoConnectionSettings
import top.ozaoza.remoe.settings.VideoSettingsStore
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : ComponentActivity(), RtcSession.Observer, RendererCommon.RendererEvents {
    private val probeExecutor = Executors.newSingleThreadExecutor()
    private lateinit var app: RemoeApplication
    private lateinit var renderer: TextureViewVideoRenderer
    private lateinit var remoteCursor: RemoteCursorView
    private lateinit var touchController: RemoteTouchController
    private lateinit var inviteInput: EditText
    private lateinit var fpsInput: EditText
    private lateinit var bitrateInput: EditText
    private lateinit var scaleInput: EditText
    private lateinit var qualityInput: EditText
    private lateinit var rateControlButton: Button
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
    private lateinit var remoteToolbar: LinearLayout
    private lateinit var remoteMenuButton: Button
    private lateinit var remoteActionsPanel: LinearLayout
    private lateinit var performanceButton: Button
    private lateinit var performanceStatsView: TextView
    private lateinit var developerPanel: LinearLayout
    private lateinit var bindingClient: AndroidBindingClient
    private lateinit var deviceIdentityStore: DeviceIdentityStore
    private lateinit var nativeSessionStore: NativeSessionStore
    private lateinit var videoSettingsStore: VideoSettingsStore
    private val bindingHandler = Handler(Looper.getMainLooper())
    private var activeBinding: ActiveBinding? = null
    private var bindingForeground = false
    private var session: RtcSession? = null
    private var remoteTrack: VideoTrack? = null
    private var selectedRateControl = VideoRateControl.CBR
    private var showPerformanceStats = false
    private var remoteMenuExpanded = false
    private var remoteModeActive = false
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
        videoSettingsStore = VideoSettingsStore(this)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT

        renderer = TextureViewVideoRenderer(this).apply {
            init(app.rtcRuntime.eglBase.eglBaseContext, this@MainActivity)
            setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            setMirror(false)
        }
        val remoteTouchLayer = View(this)
        remoteCursor = RemoteCursorView(this).apply { visibility = View.GONE }
        touchController = RemoteTouchController(
            view = remoteTouchLayer,
            contentView = renderer,
            send = { input -> session?.sendInput(input) == true },
            onPointerMoved = ::positionRemoteCursor,
        )

        controlPanel = createControlPanel()
        remoteStopButton = secondaryButton("断开").apply {
            setOnClickListener { disconnect("用户断开") }
        }
        performanceButton = secondaryButton("性能").apply {
            setOnClickListener { setPerformanceStatsVisible(!showPerformanceStats) }
        }
        remoteMenuButton = circularIconButton("⋮", "展开远程操作")
        remoteActionsPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        addRemoteAction(performanceButton)
        addRemoteAction(remoteStopButton)
        remoteToolbar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
            visibility = View.GONE
            addView(remoteMenuButton, LinearLayout.LayoutParams(dp(48), dp(48)).apply {
                gravity = Gravity.END
            })
            addView(remoteActionsPanel, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(6) })
        }
        remoteMenuButton.setOnClickListener { setRemoteMenuExpanded(!remoteMenuExpanded) }
        performanceStatsView = textView(13f, Color.WHITE).apply {
            typeface = Typeface.MONOSPACE
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setBackgroundColor(Color.argb(210, 8, 11, 16))
            visibility = View.GONE
        }
        renderPerformanceStats(RtcPerformanceStats())

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
            addView(remoteCursor, FrameLayout.LayoutParams(dp(38), dp(52), Gravity.TOP or Gravity.START))
            addView(controlPanel, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            addView(remoteToolbar, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).apply { setMargins(dp(12), dp(12), dp(12), dp(12)) })
            addView(performanceStatsView, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            ).apply { setMargins(dp(12), dp(12), dp(12), dp(12)) })
        }
        setContentView(root)
        showStatus("登录后选择一台在线电脑")
        restoreNativeSession()
    }

    private fun createControlPanel(): ScrollView {
        val storedVideoSettings = videoSettingsStore.load()
        inviteInput = editText(getString(R.string.invite_hint), InputType.TYPE_CLASS_TEXT).apply {
            isSingleLine = true
        }
        fpsInput = editText("", InputType.TYPE_CLASS_NUMBER).apply {
            setText(storedVideoSettings.fps.toString())
        }
        bitrateInput = editText("", InputType.TYPE_CLASS_NUMBER).apply {
            setText(storedVideoSettings.bitrateMbps.toString())
        }
        scaleInput = editText("", InputType.TYPE_CLASS_NUMBER).apply {
            setText(storedVideoSettings.scalePercent.toString())
        }
        qualityInput = editText("", InputType.TYPE_CLASS_NUMBER).apply {
            setText(storedVideoSettings.quality.toString())
        }
        selectedRateControl = storedVideoSettings.rateControl
        rateControlButton = secondaryButton("CBR").apply {
            setOnClickListener {
                selectedRateControl = if (selectedRateControl == VideoRateControl.CBR) {
                    VideoRateControl.FIXED_QUALITY
                } else {
                    VideoRateControl.CBR
                }
                renderRateControl()
            }
        }
        renderRateControl()
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

        val primaryVideoSettings = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(settingColumn("FPS", fpsInput), weightedColumn())
            addView(settingColumn("网络 Mbps", bitrateInput), weightedColumn(start = 8))
            addView(settingColumn("编码缩放 %", scaleInput), weightedColumn(start = 8))
        }
        val qualityVideoSettings = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(settingColumn("码控", rateControlButton), weightedColumn())
            addView(settingColumn("质量（小=好）", qualityInput), weightedColumn(start = 8))
        }
        val videoSettings = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(primaryVideoSettings, matchWrap())
            addView(qualityVideoSettings, matchWrap(top = 8))
        }
        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(connectButton, LinearLayout.LayoutParams(0, dp(52), 1f))
            addView(disconnectButton, LinearLayout.LayoutParams(0, dp(52), 1f))
        }
        developerPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(inviteInput, matchWrap(top = 10))
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
                text = "视频参数"
                typeface = Typeface.DEFAULT_BOLD
            }, matchWrap(top = 30))
            addView(textView(12f, Color.rgb(145, 154, 170)).apply {
                text = "与 Web 端一致；固定质量模式下数值越小画质越高"
            }, matchWrap(top = 4))
            addView(videoSettings, matchWrap(top = 10))
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
        val quality = qualityInput.text.toString().toIntOrNull()
        if (fps !in 1..240 || bitrateMbps !in 1..1000 || scale !in 10..100 ||
            quality !in 1..51
        ) {
            return showStatus("FPS/码率/缩放/质量参数无效", true)
        }
        val videoSettings = VideoConnectionSettings(
            fps = fps!!,
            bitrateMbps = bitrateMbps!!,
            scalePercent = scale!!,
            rateControl = selectedRateControl,
            quality = quality!!,
        )
        val config = videoSettings.toClientConfig()
        videoSettingsStore.save(videoSettings)
        diagnosticsView.text = "log: ${app.diagnosticLog.path()}"
        connectButton.isEnabled = false
        disconnectButton.isEnabled = true
        codecProbeButton.isEnabled = false
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        enterRemoteMode()
        RemoteSessionService.start(this)
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
        setPerformanceStatsVisible(false)
        session?.close(reason)
        session = null
        RemoteSessionService.stop(this)
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
        val rate = if (header.rateControl == VideoRateControl.FIXED_QUALITY) {
            "固定质量 ${header.quality} · 网络 ${header.bitrateBps / 1_000_000.0} Mbps"
        } else {
            "${header.bitrateBps / 1_000_000.0} Mbps CBR"
        }
        showStatus("${header.codec} ${header.width}×${header.height} · ${header.fpsNum} FPS · $rate")
        touchController.syncPointerPosition()
    }

    override fun onPerformanceStats(stats: RtcPerformanceStats) = postUi {
        renderPerformanceStats(stats)
    }

    override fun onDiagnostics(summary: String) = postUi {
        diagnosticsView.text = "$summary\n\n${app.diagnosticLog.snapshot()}"
    }

    override fun onError(message: String) = postUi {
        touchController.cancel()
        detachRemoteTrack()
        session = null
        RemoteSessionService.stop(this)
        setPerformanceStatsVisible(false)
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
        touchController.refreshPointerPosition()
    }

    override fun onStop() {
        bindingForeground = false
        bindingHandler.removeCallbacks(bindingPoll)
        touchController.cancel()
        if (session != null) {
            app.diagnosticLog.append("lifecycle", "Activity stopped; retaining RtcSession")
        }
        super.onStop()
    }

    override fun onStart() {
        super.onStart()
        bindingForeground = true
        if (session != null) {
            app.diagnosticLog.append("lifecycle", "Activity started; resuming retained RtcSession")
        }
        if (activeBinding != null) scheduleBindingPoll(0)
    }

    override fun onDestroy() {
        session?.close("界面已销毁，连接已断开")
        session = null
        RemoteSessionService.stop(this)
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
        remoteModeActive = true
        remoteCursor.visibility = View.INVISIBLE
        touchController.resetViewport()
        setRemoteMenuExpanded(false)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        controlPanel.visibility = View.GONE
        remoteToolbar.visibility = View.VISIBLE
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    private fun leaveRemoteMode() {
        remoteModeActive = false
        remoteCursor.visibility = View.GONE
        touchController.resetViewport()
        setRemoteMenuExpanded(false)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        remoteToolbar.visibility = View.GONE
        performanceStatsView.visibility = View.GONE
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

    private fun circularIconButton(label: String, description: String): Button = Button(this).apply {
        text = label
        textSize = 28f
        isAllCaps = false
        contentDescription = description
        setTextColor(Color.WHITE)
        backgroundTintList = null
        background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(Color.argb(230, 28, 34, 43))
            setStroke(dp(1), Color.rgb(78, 89, 104))
        }
        minWidth = 0
        minHeight = 0
        minimumWidth = 0
        minimumHeight = 0
        setPadding(0, 0, 0, 0)
    }

    private fun editText(hintText: String, type: Int): EditText = EditText(this).apply {
        hint = hintText
        inputType = type
        setTextColor(Color.WHITE)
        setHintTextColor(Color.rgb(145, 154, 170))
        backgroundTintList = null
        background = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_focused), inputBackground(Color.rgb(73, 220, 175)))
            addState(intArrayOf(), inputBackground(Color.rgb(78, 89, 104)))
        }
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(12), 0, dp(12), 0)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            textCursorDrawable = ColorDrawable(Color.WHITE)
        }
    }

    private fun inputBackground(strokeColor: Int): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(8).toFloat()
        setColor(Color.rgb(28, 34, 43))
        setStroke(dp(1), strokeColor)
    }

    private fun textView(size: Float, color: Int): TextView = TextView(this).apply {
        textSize = size
        setTextColor(color)
    }

    private fun settingColumn(label: String, control: View): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        addView(textView(12f, Color.rgb(145, 154, 170)).apply { text = label })
        addView(control, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            dp(52),
        ).apply { topMargin = dp(4) })
    }

    private fun weightedColumn(start: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply {
            marginStart = dp(start)
        }

    private fun renderRateControl() {
        val fixedQuality = selectedRateControl == VideoRateControl.FIXED_QUALITY
        rateControlButton.text = if (fixedQuality) "固定质量" else "CBR"
        qualityInput.isEnabled = fixedQuality
        qualityInput.alpha = if (fixedQuality) 1f else 0.45f
    }

    private fun setPerformanceStatsVisible(visible: Boolean) {
        showPerformanceStats = visible
        performanceButton.text = if (visible) "隐藏性能" else "性能"
        performanceStatsView.visibility = if (visible) View.VISIBLE else View.GONE
        if (!visible) renderPerformanceStats(RtcPerformanceStats())
        session?.setPerformanceStatsEnabled(visible)
    }

    private fun setRemoteMenuExpanded(expanded: Boolean) {
        remoteMenuExpanded = expanded
        remoteActionsPanel.visibility = if (expanded) View.VISIBLE else View.GONE
        remoteMenuButton.text = if (expanded) "×" else "⋮"
        remoteMenuButton.contentDescription = if (expanded) "收起远程操作" else "展开远程操作"
    }

    /** Keeps every remote-session action in the expandable top-right menu. */
    private fun addRemoteAction(button: Button) {
        remoteActionsPanel.addView(button, LinearLayout.LayoutParams(dp(120), dp(48)).apply {
            if (remoteActionsPanel.childCount > 0) topMargin = dp(6)
        })
    }

    private fun renderPerformanceStats(stats: RtcPerformanceStats) {
        performanceStatsView.text = String.format(
            Locale.US,
            "解码 FPS   %.1f\n接收码率   %.1f Mbps\n实际网速   %.1f KB/s\n丢帧事件   %d",
            stats.fps,
            stats.bitrateMbps,
            stats.dataRateKBps,
            stats.lossEvents,
        )
    }

    private fun positionRemoteCursor(point: top.ozaoza.remoe.input.TouchGestureEngine.Point) {
        remoteCursor.translationX = point.x - dp(3)
        remoteCursor.translationY = point.y - dp(2)
        if (remoteModeActive) remoteCursor.visibility = View.VISIBLE
    }

    private fun matchWrap(top: Int = 0): LinearLayout.LayoutParams = LinearLayout.LayoutParams(
        LinearLayout.LayoutParams.MATCH_PARENT,
        LinearLayout.LayoutParams.WRAP_CONTENT,
    ).apply { topMargin = dp(top) }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

}
