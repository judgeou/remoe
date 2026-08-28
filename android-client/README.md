# Remoe Android

原生 Kotlin Android 客户端工程。当前骨架使用平台 `Activity`，不包含 Compose 或第三方 UI 依赖。

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
