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
基于 EGL 的 `TextureView` 渲染器。目标 HONOR 设备的 `SurfaceViewRenderer` 能收到并解码视频帧，
但系统合成结果为黑屏；`TextureView` 路径已真机验证，并按视频宽高比完整显示、避免裁掉任务栏。

用户打开远程画面的“性能”面板时，每秒采集 inbound RTP stats，并把脱敏诊断写入应用私有目录的
`files/diagnostics/latest.log`。日志只记录候选类型和传输协议，不记录 IP、端口、SDP、ICE
candidate、invite session 或其他 token；面板关闭后停止 stats 轮询以减少功耗。首版进入后台会立即断开并按 DataChannel、
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

网页批准后，阶段 E 会继续登记本机 Android Keystore 设备密钥并建立 native session。

## 阶段 E 本地实现

Android 使用系统 Android Keystore 生成 P-256/ES256 设备密钥，私钥不可导出。登记 ceremony 只能
由已批准绑定的 client secret 领取；App 对包含 challenge、binding ID、device ID 和公钥的版本化消息
签名，服务端验证后在同一 SQLite 事务中保存设备公钥、native refresh session 并完成绑定。一个
网页账号可以关联多台 Android 设备，每台设备使用不同密钥。

refresh token 使用 Android Keystore 中的 AES-256-GCM key 加密后保存，access token 仅驻留内存；
应用重启时用 refresh token 换取新的短期 access token。refresh token 丢失时可用本机设备密钥
签名 challenge 重新登录；卸载或清除 App 数据会删除密钥，因此必须重新扫码绑定。

该流程不依赖 Credential Manager、Google Password Manager、Digital Asset Links 或 Google Play。
生产服务部署和 HONOR 真机扫码绑定、本机密钥登录验收均已完成。

## 阶段 F 本地实现

登录成功后，Android 使用内存中的短期 access token 获取账号 Host 列表并显示在线状态；点击在线
Host 会通过受认证接口领取一次性 WSS invite，再复用阶段 C 的 WebRTC VideoTrack 连接。access token
临近过期时才使用加密 refresh token 续期，不做后台定时刷新；注销会撤销服务端 refresh session 并
清除本地密文。当前保留手动 invite 输入作为开发诊断入口。

本阶段的 API、单测、lint 与 APK 构建已通过；真实账号 Host 列表、一次性 invite 和正式 WebRTC
连接已在 HONOR 真机验收。

## 阶段 G：触摸控制

远程画面使用与 Web 移动端一致的触控板指针：较大的高对比度鼠标箭头默认位于画面中心，单指滑动
相对移动指针，轻点在当前指针处左击，双击后继续滑动执行左键拖拽，双指轻点右击。双指捏合以
两指中心为锚点在 100%–400% 间缩放；放大后双指同向滑动会平移本地视口，100% 时则分别发送
水平/垂直滚轮。触摸使用稳定 pointer ID；第二根手指加入后会按中心位移与指间距变化锁定本次
手势类型，避免缩放、滚轮或抬指时误触鼠标按键。

断开、窗口失焦和 `ACTION_CANCEL` 会释放尚未抬起的远端左键，离开远程模式会重置视口。坐标、
相对移动、点击、双击拖拽、双指右击、双指滚轮、缩放锚点、平移约束和取消释放均有 JVM 单元
测试；新的触控板指针交互等待用户真机验收。

连接前的视频设置与 Web 端一致，支持 1–240 FPS、1–1000 Mbps 网络目标、10%–100% 编码缩放，
以及 CBR / 固定质量码控；固定质量范围为 1–51，数值越小画质越高。远程画面可切换性能浮层，
显示解码 FPS、接收码率、实际网速和本周期丢帧事件。远程画面默认只保留右上角圆形“更多”入口，
性能、断开及后续远程操作统一放入其展开面板。App 会在参数通过校验并发起连接时保存这组视频
参数，下次启动自动恢复；invite 和连接令牌不会写入该设置。指标计算有 JVM 单元测试，真机显示
等待用户验收。

## 自行管理 Release 签名

项目不使用 Google Play / Play App Signing。Release APK 必须始终使用同一枚自行保管的密钥；密钥
丢失后，已安装用户无法升级到新版本。运行以下脚本交互式创建密钥，密码只在 `keytool` 中输入：

```powershell
.\create-release-keystore.ps1
```

`private/` 与常见 keystore 后缀已被 Git 忽略；仍应在密码管理器保存密码，并把 keystore 加密备份
到至少两个独立位置。Release 签名只用于 APK 安装与升级身份，不参与 Android 设备密钥协议。

Release 构建只从进程环境读取签名配置：

```powershell
$env:REMOE_RELEASE_STORE_FILE = 'C:\secure\remoe-release.p12'
$env:REMOE_RELEASE_STORE_PASSWORD = '<from password manager>'
$env:REMOE_RELEASE_KEY_ALIAS = 'remoe-release'
$env:REMOE_RELEASE_KEY_PASSWORD = '<from password manager>'
.\gradlew.bat assembleRelease
```

缺少任一变量时，Release 构建会直接失败，不会悄悄生成未签名 APK。

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
