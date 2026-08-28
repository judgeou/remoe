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
