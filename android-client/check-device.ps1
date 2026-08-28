param(
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$sdkPath = if ($env:ANDROID_HOME) {
    $env:ANDROID_HOME
} elseif ($env:ANDROID_SDK_ROOT) {
    $env:ANDROID_SDK_ROOT
} else {
    Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
$adbPath = Join-Path $sdkPath 'platform-tools\adb.exe'

if (-not (Test-Path -LiteralPath $adbPath)) {
    throw "ADB not found: $adbPath"
}

& $adbPath start-server | Out-Null
$deviceLines = @(& $adbPath devices -l) | Select-Object -Skip 1 | Where-Object { $_.Trim() }

if (-not $deviceLines) {
    Write-Error 'No ADB device found. Enable USB debugging on the phone, reconnect USB, and accept the RSA authorization prompt.'
}
if ($deviceLines -match '\bunauthorized\b') {
    Write-Error 'The phone is connected but unauthorized. Unlock it and accept the USB debugging RSA prompt.'
}
if ($deviceLines -match '\boffline\b') {
    Write-Error 'The ADB device is offline. Reconnect USB and restart USB debugging.'
}

$readyDevices = @($deviceLines | Where-Object { $_ -match '\sdevice(?:\s|$)' })
if ($readyDevices.Count -ne 1) {
    throw "Expected exactly one authorized device, found $($readyDevices.Count)."
}

Write-Host "Authorized device: $($readyDevices[0])"
& $adbPath shell getprop ro.product.manufacturer
& $adbPath shell getprop ro.product.model
& $adbPath shell getprop ro.build.version.release
& $adbPath shell getprop ro.build.version.sdk

if ($Install) {
    $apkPath = Join-Path $PSScriptRoot 'app\build\outputs\apk\debug\app-debug.apk'
    if (-not (Test-Path -LiteralPath $apkPath)) {
        throw "Debug APK not found: $apkPath. Run .\gradlew.bat assembleDebug first."
    }
    & $adbPath install -r $apkPath
    if ($LASTEXITCODE -ne 0) { throw 'APK installation failed.' }
    & $adbPath shell am start -n top.ozaoza.remoe/.MainActivity
    if ($LASTEXITCODE -ne 0) { throw 'Activity launch failed.' }
}
