# Remoe Android 原生客户端设计方案

> 状态：设计基线，2026-08-28  
> 目标读者：后续开发者与新的 Codex 对话  
> 对应仓库：`C:\Users\judge\projects\remoe`  
> Android 工程：`android-client/`

> 实施进度：阶段 A–D 已完成代码与自动化测试；阶段 C 的完整 10 分钟长稳测试延后至阶段 H，
> 阶段 D 尚未部署生产环境或做真机端到端绑定。下一步从阶段 E 开始。

## 1. 目标

开发一个原生 Android 客户端，功能与 Windows `remoe_client` 和网页客户端基本一致：

- 绑定并登录现有 Remoe 网页账号；
- 显示账号下的在线 Host；
- 使用标准 WebRTC VideoTrack 接收远程桌面；
- 通过可靠有序 DataChannel 发送配置、输入和剪贴板；
- 支持 AV1/H.264 硬件解码、触控、物理键盘和诊断日志；
- 保持 STUN-only，不部署或使用 TURN/其他中继；
- 与当前 protocol v11 host 和信令服务兼容。

Android 端业务代码使用 Kotlin。允许引入预编译 `libwebrtc.aar`；项目不自行维护 C++、JNI
或 NDK 构建。AAR 内部自带的原生 `.so` 不视为项目自行开发 NDK 代码。

## 2. 已确定的技术决策

| 领域 | 决策 |
|---|---|
| 视频传输 | 标准 WebRTC RTP/SRTP VideoTrack |
| 控制传输 | `remoe-control` 可靠、有序 DataChannel |
| WebRTC 实现 | 上游 `org.webrtc` API + 固定版本预编译 Android AAR |
| ICE | STUN-only；不配置 TURN，不提供 relay fallback |
| 信令 | 现有 WSS bootstrap；只交换 SDP/ICE/ready/ack |
| 视频解码 | 优先 Android MediaCodec 硬件解码；通过 libwebrtc decoder factory 使用 |
| Android UI | 单 Activity 为主，传统 View；视频使用 `SurfaceViewRenderer` |
| 状态管理 | ViewModel + `StateFlow` + 单向数据流 |
| 首次账号绑定 | 已登录网页生成二维码，Android 扫码后绑定新 Passkey |
| 后续登录 | Android Credential Manager 直接使用 Passkey |
| 网页与 APK 关联 | Digital Asset Links |
| 本地凭据 | Android Keystore 支持的加密存储，不明文保存 refresh token |
| 最低系统 | API 26；Passkey 功能实际要求 API 28+，低版本需给出明确提示/降级路径 |

## 3. 当前工程与环境状态

### 3.1 Android 工程

已创建 `android-client/`，当前是可编译、可安装的 Kotlin Android 项目：

- project name：`RemoeAndroid`
- application ID / namespace：`top.ozaoza.remoe`
- minSdk：26
- compileSdk / targetSdk：35
- JDK：17
- Gradle Wrapper：8.12.1
- Android Gradle Plugin：8.10.1
- Kotlin plugin：2.1.20

当前入口为：

```text
android-client/app/src/main/java/top/ozaoza/remoe/MainActivity.kt
```

已有辅助脚本：

```powershell
cd C:\Users\judge\projects\remoe\android-client
.\check-device.ps1 -Install
```

该脚本检查 ADB 授权，可安装 Debug APK 并启动 `MainActivity`。

### 3.2 本机 Android 环境

- Android Studio 已安装；
- Android SDK：`C:\Users\judge\AppData\Local\Android\Sdk`；
- 已安装 platform：API 35、36.1；
- 已安装 build-tools：34.0.0、35.0.0、35.0.1、36.0.0；
- ADB 35.0.2；
- `platform-tools` 已加入当前 Windows 用户的持久化 PATH；
- 当前 Codex 桌面进程在 PATH 修改前启动，因此其 shell 有时仍需使用 ADB 绝对路径；新终端可直接运行 `adb`。

### 3.3 已连接测试设备

- 厂商：HONOR
- 型号：AAP-AN00
- Android：16
- API：36
- SoC 平台：Qualcomm `canoe`
- GPU：Adreno 840
- ADB 序列号：`AVXB6R6108002385`
- ADB 已授权，Debug APK 已安装并成功启动；进程、前台 Activity 和日志均正常。

设备的 MediaCodec 配置明确包含：

```text
c2.qti.av1.decoder
c2.qti.av1.decoder.low_latency
c2.qti.avc.decoder
c2.qti.avc.decoder.low_latency
```

这证明设备具备 AV1 和 H.264 低延迟硬件解码能力，但仍必须通过 libwebrtc
`DefaultVideoDecoderFactory.getSupportedCodecs()` 实机确认选定 AAR 确实暴露 AV1。

### 3.4 服务器状态

生产域名目前已经恢复并实测正常：

```text
https://remoe.oza-oza.top/
https://remoe.oza-oza.top/healthz
```

两者均返回 HTTP 200。服务器可通过以下方式登录：

```bash
ssh root@remoe.oza-oza.top
```

已确认 SSH root 公钥认证可用。服务器布局：

```text
/opt/remoe/signaling-server/current/server.mjs
/opt/remoe/web-client/current/
/etc/caddy/Caddyfile
```

服务：

```text
remoe-signaling.service   active/running
caddy.service             active/running
Node                      127.0.0.1:8080
Caddy                     0.0.0.0:80, *:443
```

后续部署应保持现有 release/current 目录结构，先上传新 release、验证，再原子切换 `current`
软链接并重启/重载相应服务；不要直接覆盖正在运行的 current 文件。

## 4. Android 客户端模块设计

建议先保持单 app module，在代码层分包；出现明确复用/构建收益后再拆 Gradle module。

```text
top.ozaoza.remoe/
  app/
    RemoeApplication.kt

  ui/
    launcher/
      LauncherActivity.kt
      LauncherViewModel.kt
      LauncherUiState.kt
    binding/
      QrBindingActivity.kt
      QrBindingViewModel.kt
    stream/
      StreamActivity.kt
      StreamViewModel.kt
      StreamUiState.kt

  auth/
    AccountApi.kt
    PasskeyClient.kt
    QrBindingClient.kt
    TokenStore.kt
    SessionRepository.kt

  signaling/
    SignalWebSocket.kt
    SignalFrameCodec.kt

  protocol/
    Protocol.kt
    ClientConfigCodec.kt
    StreamHeaderCodec.kt
    InputEventCodec.kt
    ClipboardCodec.kt
    ClockSyncCodec.kt

  rtc/
    RtcEngine.kt
    RtcSession.kt
    RtcSessionObserver.kt
    VideoSinkController.kt
    RtcStatsCollector.kt

  input/
    RemoteInputView.kt
    GestureInterpreter.kt
    AndroidKeyMapper.kt

  diagnostics/
    DiagnosticLog.kt
    DiagnosticExporter.kt
```

### 4.1 生命周期原则

- `PeerConnectionFactory` 和根 `EglBase` 由 application scope 持有；
- 每次连接创建独立 `RtcSession`、`PeerConnection` 和 DataChannel；
- `SurfaceViewRenderer` 属于 Activity/View 生命周期；Track 与 renderer 必须显式 attach/detach；
- 所有 `PeerConnection`、Track、renderer、EGL 对象都必须有确定的 `dispose/release` 顺序；
- WebRTC callback 不直接操作 View，通过线程安全状态/事件交给 ViewModel/UI；
- 第一版进入后台即主动断开，不做后台长连接和 foreground service；
- 播放界面启用 `FLAG_KEEP_SCREEN_ON`，使用沉浸式横屏；
- 网络切换第一版采取明确断开并重连，不实现隐式 ICE restart。

## 5. libwebrtc 集成

候选预编译依赖：

```kotlin
implementation("io.github.webrtc-sdk:android:144.7559.09")
```

项目地址：<https://github.com/webrtc-sdk/android>。该项目不是 Google 官方 Maven 发布，
但提供近期 Chromium libwebrtc 的预编译 AAR。版本必须固定，不能使用动态版本。

正式采用前必须：

1. 下载并记录 AAR SHA-256；
2. 保存对应 Chromium/libwebrtc revision 和许可证；
3. 真机打印 `DefaultVideoDecoderFactory.getSupportedCodecs()`；
4. 确认包含 AV1；
5. 建立真实 VideoTrack，确认最终使用 `c2.qti.av1.decoder.low_latency` 或等效硬件 decoder；
6. 对大幅画面变化、丢包、前后台切换进行长时间测试。

初始化轮廓：

```kotlin
PeerConnectionFactory.initialize(
    PeerConnectionFactory.InitializationOptions.builder(applicationContext)
        .setEnableInternalTracer(false)
        .createInitializationOptions()
)

val eglBase = EglBase.create()
val decoderFactory = DefaultVideoDecoderFactory(eglBase.eglBaseContext)
val factory = PeerConnectionFactory.builder()
    .setVideoDecoderFactory(decoderFactory)
    .createPeerConnectionFactory()
```

第一项 Android 开发任务不是连接服务器，而是做 codec probe 页面，显示：

- libwebrtc 版本/构建信息；
- 支持的 codec 列表和 fmtp；
- Android MediaCodec AV1/H.264 decoder 列表；
- low-latency feature 是否可用；
- EGL/GPU 信息。

## 6. WebRTC 会话设计

Android 角色与现有 Windows/Web client 相同：client 是 offerer。

### 6.1 建连顺序

1. 创建 `PeerConnection`；
2. 配置 STUN server，不配置 TURN；
3. 创建可靠有序 `remoe-control` DataChannel；
4. 添加 `RECV_ONLY` video transceiver；
5. 创建并设置 local offer；
6. 通过现有 WSS `/signal#sessionId` 发送 WRMS SDP/ICE；
7. 收到并设置 remote answer/candidates；
8. 收到远端 VideoTrack 后 attach 到 `SurfaceViewRenderer`；
9. DataChannel open 后发送 protocol v11 `ClientConfig`；
10. 收到并验证 `StreamHeader`；
11. renderer、解码队列和 UI 准备完成后发送 `StreamReady`；
12. 开始输入、剪贴板、时钟同步和 stats 采集。

### 6.2 ICE 配置

- 只接受 `stun:` URL；
- 不在配置中加入任何 `turn:`/`turns:` URL；
- `iceTransportPolicy` 使用允许 host/srflx 的正常策略，而不是 relay；
- 没有直连路径时明确失败并显示候选/ICE 状态，不偷偷走中继；
- 首版复用 host/网页当前 STUN 设置。

### 6.3 VideoTrack 渲染

- 使用 `SurfaceViewRenderer` 和共享 EGL context；
- VideoTrack 通过 `addSink(renderer)` 连接；
- 不自行接收 RTP、不自行组 AV1 OBU、不自行调用 MediaCodec；
- RTP packet buffer、NACK、PLI、jitter buffer 和解码调度交给完整 libwebrtc；
- 正确设置 scaling type；远程控制默认适配窗口并保留宽高比；
- stats 中必须显示实际 codec、decoder implementation、分辨率和 FPS。

## 7. protocol v11

权威定义仍是：

```text
src/protocol.h
README.md 的 WebRTC 传输协议 v11 章节
```

Android 端必须使用显式 little-endian 编解码，绝不能依赖 Kotlin/JVM 对象内存布局：

```kotlin
ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
```

主要消息：

| 消息 | 大小 | 方向 | 用途 |
|---|---:|---|---|
| `WebRtcSignalHeader` | 20 | 双向 WSS | SDP/ICE/ready/ack framing |
| `ClientConfig` | 36 | client → host | FPS、网络码率、缩放、码控、剪贴板能力 |
| `StreamHeader` | 44 | host → client | codec、分辨率、FPS、码率、profile、码控 |
| `StreamReady` | 8 | client → host | client 解码/显示准备完成 |
| `ClockSyncRequest` | 24 | client → host | 时钟同步请求 |
| `ClockSyncResponse` | 40 | host → client | host 接收/发送时间 |
| `InputEvent` | 24 | client → host | 鼠标和 Windows scan code 键盘 |
| `ClipboardHeader` | 16 + text | 双向 | 最大 1 MiB UTF-8 文本剪贴板 |

所有协议 codec 都要使用与 Web/C++ 相同的 golden byte fixtures。至少测试：

- magic/version/header size；
- CBR 与固定质量 ClientConfig；
- AV1/H.264 StreamHeader；
- 有符号输入值；
- UTF-8 剪贴板长度与上限；
- 截断、超长、错误版本和保留位拒绝；
- WRMS 分片/合并输入的 frame buffer。

## 8. 扫码绑定同一网页账号的 Passkey

### 8.1 用户目标

第一次：

1. 用户在网页上登录现有 Remoe 账号；
2. 网页生成 Android 绑定二维码；
3. Android 扫码；
4. 网页确认当前 Android 设备；
5. Android Credential Manager 为 `remoe.oza-oza.top` 创建一枚新 Passkey；
6. 服务端把新 Passkey 保存到当前网页账号的同一个 `user_id`；
7. Android 同时获得自己的 native refresh session。

以后：

1. Android 请求 authentication options；
2. Credential Manager 弹出 Passkey/生物识别/锁屏确认；
3. Android 将 assertion 发给服务器；
4. 服务端通过 credential ID 找到同一网页账号；
5. 服务端签发 Android access/refresh token；
6. Android 直接进入 Host 列表，不再扫码。

Passkey 可能由密码管理器同步到其他设备，不能宣传为“私钥永远锁死当前物理手机”。
“当前 Android 客户端设备”应通过独立 native session/refresh token 记录；Passkey 负责账号身份。

### 8.2 安全绑定流程

推荐状态机：

```text
CREATED → CLAIMED → APPROVED → PASSKEY_CREATED → COMPLETED
                   ↘ REJECTED
任何状态 → EXPIRED
```

详细步骤：

1. 已登录网页调用 bind/start；
2. 服务端创建绑定记录，关联当前 `user_id`，生成 256-bit 随机 QR token；
3. 网页显示 QR，默认 2 分钟过期；
4. Android 扫码后生成独立 client secret，并调用 bind/claim；
5. 服务端只保存 QR token/client secret 的哈希；
6. 网页显示请求绑定的设备名、型号和短比较码；
7. 用户在网页明确批准；
8. Android 持 client secret 轮询，批准后领取一次性 registration options；
9. Android 调 Credential Manager 创建 Passkey；
10. 服务端验证 challenge、RP ID、Android origin、UV/UP 和 credential 唯一性；
11. Passkey 保存到绑定记录的同一 `user_id`；
12. 服务端创建 native session，返回 access/refresh token；
13. 立即删除绑定/ceremony secret，二维码不可再次使用。

二维码建议使用自描述 URI：

```text
remoe://bind?v=1&server=https%3A%2F%2Fremoe.oza-oza.top&token=<random>
```

二维码中禁止包含：

- access token；
- refresh token；
- Passkey private key；
- 长期设备 token；
- 可直接识别账号的 user ID/email；
- 超过两分钟仍有效的 bearer credential。

### 8.3 建议新增服务端接口

接口名可在实现时调整，但职责必须保持分离：

```text
POST /api/android/bind/start                 # 网页登录态
POST /api/android/bind/claim                 # Android + QR token
GET  /api/android/bind/status                # 网页轮询 claim/完成状态
POST /api/android/bind/approve               # 网页登录态明确批准
POST /api/android/bind/reject                # 网页拒绝

POST /api/android/passkey/register/options   # Android + client secret
POST /api/android/passkey/register/verify    # Android credential response

POST /api/android/passkey/login/options      # Android 直接登录
POST /api/android/passkey/login/verify       # 验证 assertion 并签发 token
```

Android ceremony 不能依赖浏览器 Cookie。服务端应返回独立的 opaque ceremony ID/secret，
保存 challenge、用途、绑定 user ID、过期时间和预期 origin，并在成功/失败后消费。

### 8.4 与现有数据库的关系

现有信令服务已经支持一个用户拥有多枚 Passkey。Android 注册成功后，继续复用现有
passkey record 格式，把新 credential 保存到绑定网页账号的同一 `user_id`：

```text
Web passkey A ─┐
Web passkey B ─┼─ same user_id
Android key  C ┘
```

建议为 passkey 增加可选的人类可读标签/来源信息，例如：

```text
HONOR AAP-AN00 · Android
```

设备名称是显示信息，不能作为安全身份依据。

## 9. Digital Asset Links 与 Android origin

原生 Credential Manager 为网页 RP 创建/使用 Passkey 时，Digital Asset Links 必须部署。

目标文件：

```text
https://remoe.oza-oza.top/.well-known/assetlinks.json
```

响应要求：

- HTTP 200；
- `Content-Type: application/json`；
- 不允许 301/302；
- 公网可访问；
- `robots.txt` 不阻止 `/.well-known/`。

声明结构：

```json
[
  {
    "relation": [
      "delegate_permission/common.handle_all_urls",
      "delegate_permission/common.get_login_creds"
    ],
    "target": {
      "namespace": "android_app",
      "package_name": "top.ozaoza.remoe",
      "sha256_cert_fingerprints": [
        "<RELEASE_CERT_SHA256>"
      ]
    }
  }
]
```

当前本机 Debug certificate SHA-256：

```text
06:A5:C2:F1:63:6A:22:76:84:64:55:FB:A5:00:D6:D0:6A:41:A6:96:03:69:45:89:F4:02:31:3A:19:20:EA:DB
```

对应 Android WebAuthn origin：

```text
android:apk-key-hash:BqXC8WNqInaEZFX7pQDW0GpBppYDaUWJ9AIxOhkg6ts
```

生产服务器正式上线时必须使用 Release/Play App Signing certificate。不要把 Debug key 当作永久生产信任根。
若需要在生产域名临时测试 Debug APK，应明确记录、限制测试周期，并在 Release 验证完成后删除 Debug 指纹。

服务端应按 ceremony 类型验证 origin：

- 网页 WebAuthn：`https://remoe.oza-oza.top`；
- Android Debug：上面的 `android:apk-key-hash:...`；
- Android Release：由 Release signing certificate 计算；
- 未知 origin 一律拒绝；
- 不能为了兼容而跳过 origin、RP ID、challenge 或 user verification 检查。

官方参考：

- <https://developer.android.com/identity/credential-manager/prerequisites>
- <https://developer.android.com/identity/passkeys/create-passkeys>
- <https://developers.google.com/identity/passkeys/developer-guides/server-registration>
- <https://developers.google.com/identity/passkeys/developer-guides/server-authentication>

## 10. Android Passkey 实现

依赖版本在编码时查 AndroidX stable release 并固定，不使用动态版本。需要：

```text
androidx.credentials:credentials
androidx.credentials:credentials-play-services-auth（按设备兼容需求决定）
```

注册：

```text
server registration options JSON
→ CreatePublicKeyCredentialRequest
→ CredentialManager.createCredential()
→ CreatePublicKeyCredentialResponse.registrationResponseJson
→ server register/verify
```

登录：

```text
server authentication options JSON
→ GetPublicKeyCredentialOption
→ CredentialManager.getCredential()
→ PublicKeyCredential.authenticationResponseJson
→ server login/verify
```

必须处理并向用户说明：

- 用户取消；
- 没有凭据提供程序；
- 没有匹配 Passkey；
- 设备无安全锁屏；
- DAL 验证失败；
- challenge/二维码过期；
- 网络中断；
- Passkey 已注册；
- 服务端 origin/RP ID 拒绝。

## 11. Token 与本地账号状态

- access token 只保存在内存，过期后用 refresh token 更新；
- refresh token 使用 Android Keystore 支持的加密方案保存；
- 不使用明文 SharedPreferences；
- logout 调服务端撤销 native session，然后删除本地 token；
- refresh 失败时回到 Passkey 登录页；
- 每台 Android 客户端有独立 refresh session，服务端记录 client name、创建时间和最近使用时间；
- 后续网页账号页应支持查看和撤销 Android 客户端 session；
- Passkey 删除与 native session 撤销是两个动作，不应混为一个操作。

## 12. 二维码扫描

本需求是“Android 扫网页二维码”，因此需要相机扫描能力。

候选实现：

- CameraX + ML Kit Barcode Scanning；或
- ZXing Android Embedded。

选择标准：

- 是否依赖 Google Play Services；
- HONOR 测试机兼容性；
- APK 体积；
- 相机权限和生命周期管理；
- 只接受二维码，不解析任意条码；
- 扫描结果严格验证 scheme、version、HTTPS server origin 和 token 长度；
- 禁止二维码覆盖任意服务器地址，或必须弹出清晰的服务器域名确认，防止钓鱼绑定。

生产模式建议只接受配置/allowlist 中的 `https://remoe.oza-oza.top`，开发 build 才允许自定义 origin。

## 13. 输入设计

当前 protocol v11 `InputEvent` 使用 Windows 绝对鼠标坐标和 Windows scan code。

第一版手势：

| Android 操作 | 远端行为 |
|---|---|
| 单指移动/点击位置 | 绝对鼠标移动 |
| 单击 | 左键点击 |
| 长按 | 右键点击 |
| 按住后拖动 | 左键拖动 |
| 双指垂直滑动 | 垂直滚轮 |
| 双指水平滑动 | 水平滚轮 |
| 外接鼠标按键/滚轮 | 映射对应 InputEvent |
| 物理键盘 | Android KeyEvent → Windows scan code 映射 |

输入 View 使用 `MotionEvent.actionMasked` 和稳定 pointer ID；必须处理 `ACTION_CANCEL`，在断开、失焦
或 Activity pause 时释放所有按下的按钮/按键，防止远端卡键。

### 13.1 软键盘与文本输入缺口

Android IME 输出的是 Unicode 文本/组合输入，不应强行映射为 Windows scan code。中文、日文、Emoji
等无法靠当前 Keyboard InputEvent 正确表达。

可选方案：

1. 第一版通过现有 UTF-8 剪贴板发送文本并提示用户粘贴；
2. protocol 后续增加明确的 UTF-8 text input 消息，由 host 使用合适的 Windows 文本注入路径；
3. 物理键盘继续使用 scan code。

不要把软键盘字符逐个伪装成美式键盘 scan code。

Android 触控参考：

- <https://developer.android.com/develop/ui/views/touch-and-input/gestures/detector>
- <https://developer.android.com/develop/ui/views/touch-and-input/gestures/multi>
- <https://developer.android.com/develop/ui/views/touch-and-input/input-events>

## 14. 剪贴板

- 首版只支持 UTF-8 文本，与现有 protocol 一致；
- Android 读取剪贴板受系统前台/隐私限制，不能后台轮询复制 Windows 行为；
- 推荐由用户点击“发送本机剪贴板”和“接收远端剪贴板”；
- 收到远端文本时显示明确提示，再写入系统剪贴板；
- 保持 1 MiB 上限；
- 使用 sequence 防止本机与远端回环重复发送；
- 不支持图片、文件和任意 MIME object。

## 15. 诊断与可观测性

诊断必须从第一条真实 WebRTC 连接开始实现，而不是出现卡顿后再补。

记录：

- build/version、Android 版本、设备型号；
- libwebrtc revision/AAR version/hash；
- decoder factory 支持 codec；
- 最终协商 codec/fmtp/profile；
- 最终 decoder implementation；
- PeerConnection/ICE/gathering/signaling state；
- local/remote candidate type、protocol 和 network type（不记录长期凭据）；
- DataChannel open/close/error 和消息计数；
- inbound-rtp packets/bytes/lost/jitter；
- frames received/decoded/dropped；
- keyframes decoded、freeze count；
- jitter buffer delay/emitted count；
- current resolution/FPS；
- estimated frame age/clock sync RTT；
- renderer FPS 和 UI 卡顿；
- lifecycle attach/detach/dispose 顺序；
- 最近一次 PLI/NACK/ICE failure/reconnect 原因。

提供：

- 屏幕内实时 diagnostics overlay；
- 本地滚动日志文件；
- 一键导出/分享脱敏日志；
- Debug build 可启用 libwebrtc logging；Release 默认降低详细度；
- 日志禁止包含 access/refresh token、binding token、device secret、Passkey response 完整内容或 Host token。

## 16. 已知安全问题：Host token 进入 Caddy 错误日志

服务器现有 Host WebSocket 认证把 Host ID/token 放在 `Sec-WebSocket-Protocol`。当上游 Node
短暂不可用时，Caddy reverse proxy 错误日志会记录完整请求头，因此 Host token 可能进入 journal。

后续必须处理：

1. 修改 host/server：WebSocket 建立后用第一条应用层消息认证；
2. 握手的 subprotocol 不再包含长期 token；
3. 确认 Caddy/error logger 不记录敏感认证数据；
4. 部署新协议后轮换已可能写入日志的 Host token；
5. 清理/控制旧日志保留；
6. 不在文档、测试输出或诊断包中复制当前 token。

未经明确安排不要直接旋转生产 Host token，因为旋转会导致现有 Host 断开并需要重新配对。

## 17. 测试策略

### 17.1 JVM unit tests

- protocol golden bytes；
- WRMS frame buffer 分片/合并；
- QR URI parser；
- server origin allowlist；
- token/状态机错误路径；
- Android KeyEvent → Windows scan code；
- touch coordinate normalization；
- stats 派生指标计算。

### 17.2 信令服务测试

- 只有已登录网页能创建绑定；
- QR token 高熵、只保存哈希、短期过期；
- 未批准 claim 不能获得 registration options；
- 抢先/重复 claim 被拒绝或由网页明确看到；
- approve/reject/expire 状态转换；
- ceremony 与 user/binding/client secret/origin 强绑定；
- Android Passkey 保存到正确 user ID；
- credential ID 全局唯一；
- Android login 找回正确账号；
- replay response/重复 verify 被拒绝；
- 未知 APK origin、错误 RP ID、错误 challenge、UV=false 均拒绝；
- refresh/logout/revoke 流程；
- 并发和容量上限，定时清理过期状态。

### 17.3 Android instrumentation / 真机测试

- QR 相机授权允许/拒绝/永久拒绝；
- 扫描合法、错误、过期、钓鱼域名二维码；
- Credential Manager 创建/取消/无 provider；
- Passkey 后续直接登录；
- 绑定后 Host 列表；
- AV1 VideoTrack 硬解码；
- 10/30/60 分钟播放；
- 静止到大幅变化；
- Wi-Fi 抖动、丢包、切换网络；
- 前后台、锁屏、旋转、Activity 重建；
- 断开时释放按键、Track、renderer、PeerConnection 和 EGL；
- 日志导出不包含 secret。

### 17.4 服务端部署验收

- `assetlinks.json`：200、JSON MIME、无 redirect；
- `/healthz`：200；
- 网页现有 Passkey 登录不回归；
- Windows native device authorization 不回归；
- Host WSS 不回归；
- 新 Android bind/login 流程通过；
- systemd active；
- Caddy config validate；
- 部署前后 SQLite 备份和 schema migration 可回滚。

## 18. 分阶段实施计划

### 阶段 A：libwebrtc 能力探针

- 引入并锁定预编译 AAR；
- 保存许可证/revision/hash；
- 初始化 EGL/PeerConnectionFactory；
- 真机显示支持 codec；
- 验证 AV1 和低延迟硬件 decoder。

验收：AAP-AN00 上明确看到 AV1，并能创建 decoder，不崩溃。

### 阶段 B：protocol v11 Kotlin codec

- 实现所有消息 codec；
- 复用 Web/C++ golden vectors；
- 完整异常输入测试。

验收：Android 编码结果逐字节等于现有实现。

### 阶段 C：最小 VideoTrack 闭环

- 临时开发连接页；
- WSS SDP/ICE；
- DataChannel handshake；
- StreamHeader/StreamReady；
- SurfaceViewRenderer；
- stats/诊断。

验收：真实 host 连续播放至少 10 分钟，大幅变化无花屏/冻结，日志可分析。

### 阶段 D：网页二维码绑定状态机

- server bind API 和测试；
- web 生成 QR、显示 claim 设备、批准/拒绝；
- Android 扫码和 claim UI；
- 暂不创建 Passkey，先完成安全状态机。

验收：过期、抢扫、拒绝、重复使用均正确。

### 阶段 E：Credential Manager + DAL

- 确定 Release signing strategy；
- 部署 assetlinks；
- Android register/login；
- server Android origin allowlist；
- 保存到同一 user ID；
- native session/token store。

验收：网页扫码绑定一次，卸载前后按预期登录；Android 可直接 Passkey 登录同账号。

### 阶段 F：Host 列表和正式连接

- token refresh/logout；
- Host 列表、在线状态、连接参数；
- 获取 invite 并启动 RtcSession；
- 错误/重连 UX。

### 阶段 G：输入、剪贴板与体验

- 触控/鼠标/物理键盘；
- 文本剪贴板；
- 沉浸横屏；
- diagnostics overlay；
- 设置持久化。

### 阶段 H：稳定性、安全和发布

- 长稳/弱网/生命周期测试；
- Host token 握手安全改造与轮换；
- Release signing/Play signing；
- 移除生产 DAL 中不需要的 Debug cert；
- R8、依赖许可证、隐私说明；
- 灰度发布和回滚流程。

## 19. 推荐的下一次开发任务

新对话开始后，优先执行阶段 A，不要先实现扫码 UI：

1. 读取本文档；
2. 检查 `android-client/` 当前 Git 状态；
3. 加入固定版 libwebrtc AAR；
4. 编写 `RtcCodecProbe`；
5. 真机运行并记录 `DefaultVideoDecoderFactory` 支持列表；
6. 确认 AV1 decoder 后提交一个独立 commit；
7. 再进入 protocol v11 Kotlin codec。

原因：手机 MediaCodec 支持 AV1 不等于选定的 libwebrtc AAR 编译时启用了 AV1。这个事实必须最先
通过运行代码验证，否则后续 VideoTrack、扫码和完整 UI 都可能建立在错误依赖上。

## 20. 尚未最终决定的事项

- 最终 libwebrtc AAR 版本及供应链固定方式；
- Release signing/Play App Signing 方案；
- Debug APK 是否临时加入生产 DAL，或使用单独测试策略；
- QR scanner 采用 CameraX + ML Kit 还是 ZXing；
- Android API 26–27 的无 Passkey 降级行为；
- 是否扩展 protocol 支持 UTF-8 text input；
- 后台/画中画/平板/折叠屏支持范围；
- STUN server 最终配置来源和多地址策略；
- 网络切换是否在后续加入 ICE restart；
- native session 管理是否加入网页设备撤销 UI；
- Host token 从 WebSocket subprotocol 迁移到应用层认证的协议版本。

这些事项不阻塞阶段 A–C，但阶段 E/H 前必须明确。
