# Remoe Android

原生 Kotlin Android 客户端工程。阶段 A 提供 libwebrtc codec、MediaCodec 和 EGL/GPU
能力探针。启动 Debug APK 后会自动执行检测；顶部 `RESULT` 应在支持 AV1 的设备上显示 `PASS`。

libwebrtc 固定版本、校验值、revision 与许可证记录见
`third_party/webrtc-sdk-android/README.md`。

## 阶段 A 真机结果

2026-08-28 在 HONOR AAP-AN00（Android 16 / API 36）上验证通过：

```text
RESULT: PASS — low-latency AV1 decoder 已初始化 (c2.qti.av1.decoder.low_latency)
EGL vendor: Qualcomm
EGL renderer: Adreno (TM) 840
```

探针使用 `DefaultVideoDecoderFactory` 显示实际协商 codec/fmtp 列表，并通过一个仅接受
`.low_latency` MediaCodec 的 `HardwareVideoDecoderFactory` 执行 AV1 `initDecode(1920×1080)`。
这样既验证了所选 AAR 编译时包含 AV1，也验证了目标设备上的 libwebrtc → MediaCodec
低延迟硬件解码路径能够创建和初始化。

## 阶段 B protocol v11

`app/src/main/java/top/ozaoza/remoe/protocol/` 使用显式 little-endian 编解码全部 protocol v11
控制消息，包括有状态的 WRMS 分片/合并 frame buffer。JVM 测试使用来自现有 Web/C++ 布局的
硬编码 golden bytes，并覆盖错误 magic/version/header size、保留位、截断、超长和非法 UTF-8。

```powershell
.\gradlew.bat testDebugUnitTest
```

## 阶段 C 开发连接页

启动 APK 后可粘贴临时 Host invite，建立 STUN-only WebRTC 会话。Android 作为 offerer 创建
recv-only AV1/H.264 transceiver 和可靠有序的 `remoe-control` DataChannel，完成 WRMS
ready/ack、ClientConfig、StreamHeader、StreamReady 后把远端 VideoTrack 连接到
`SurfaceViewRenderer`。

连接期间每秒采集 inbound RTP stats，并把脱敏诊断写入应用私有目录的
`files/diagnostics/latest.log`。日志只记录候选类型和传输协议，不记录 IP、端口、SDP、ICE
candidate、invite session 或其他 token。首版进入后台会立即断开并按 DataChannel、
PeerConnection、WSS 的顺序释放会话资源。

个人热点场景需要启用 libwebrtc 的 any-address candidate gathering，使未被 Android
`ConnectivityManager` 枚举的 tethering 接口也能形成 peer-reflexive ICE 路径。未启用时双方
可以交换 candidate 和 STUN 检查，但无法选出成功的 candidate pair。

2026-08-28 在 HONOR AAP-AN00 与连接其个人热点的 Windows Host 上完成真实闭环：ICE 进入
`COMPLETED`，DataChannel ready/ack 成功，`c2.qti.av1.decoder` 解码 2880×1800 AV1 VideoTrack。
按开发指令在约 6 分钟时主动结束长稳测试；结束前累计接收约 398 MB、333572 个 RTP 包，网络
丢包为 0，解码 9418 帧、丢弃 31 帧。设计文档要求的完整 10 分钟长稳验收仍留待阶段 H 补跑。

## 阶段 D 网页二维码绑定

服务端使用 SQLite 持久化两分钟绑定状态机；二维码 token 与 Android 独立生成的 client secret
都只保存 SHA-256。`claim` 通过数据库条件更新完成原子竞争，网页在显示设备名称、型号和短核对码
后才能批准或拒绝。服务端测试覆盖过期、并发抢扫、拒绝和二维码重复使用。

已登录网页的“绑定 Android 手机”面板可生成二维码并轮询 claim。Android 使用 CameraX 和随 APK
打包的 ML Kit QR 模型扫码，不依赖 Google Play Services 下载；相机只在扫码 Activity 位于前台时
运行，识别成功立即关闭。绑定状态轮询也只在主 Activity 前台、且状态未结束时运行。

本阶段在网页批准后停止，界面会提示阶段 E 再创建本机 passkey。未部署生产服务，也未进行真机
绑定，以免提前引入 Credential Manager/Digital Asset Links 的半成品流程。

## 本机命令

```powershell
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
.\gradlew.bat assembleDebug
.\gradlew.bat installDebug
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" shell am start -n top.ozaoza.remoe/.MainActivity
```

建议直接用 Android Studio 打开本目录。连接实体设备后，设备应在以下命令中显示为 `device`：

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" devices -l
```
