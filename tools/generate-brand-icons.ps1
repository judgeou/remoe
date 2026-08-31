[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceSvg = Join-Path $repositoryRoot 'android-client\design\app-icon.svg'
$webPublic = Join-Path $repositoryRoot 'web-client\public'
$windowsIcon = Join-Path $repositoryRoot 'src\remoe.ico'
$workingDirectory = Join-Path $env:TEMP 'remoe-brand-icon-generation'

$chromeCandidates = @(
    (Join-Path $env:ProgramFiles 'Google\Chrome\Application\chrome.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe')
)
$browser = $chromeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$ffmpeg = Get-Command ffmpeg -ErrorAction Stop

if (-not $browser) {
    throw 'Chrome or Edge is required to render the canonical SVG icon.'
}
if (-not (Test-Path -LiteralPath $sourceSvg -PathType Leaf)) {
    throw "Canonical icon not found: $sourceSvg"
}

New-Item -ItemType Directory -Path $workingDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $webPublic -Force | Out-Null

$rendered512 = Join-Path $workingDirectory 'remoe-512.png'
$browserProfile = Join-Path $workingDirectory 'browser-profile'
$sourceUri = [Uri]::new($sourceSvg).AbsoluteUri
$browserProcess = Start-Process -FilePath $browser -ArgumentList @(
    '--headless=new',
    '--disable-gpu',
    '--no-first-run',
    '--disable-default-apps',
    '--hide-scrollbars',
    '--default-background-color=00000000',
    '--force-device-scale-factor=1',
    "--user-data-dir=$browserProfile",
    '--window-size=512,512',
    "--screenshot=$rendered512",
    $sourceUri
) -NoNewWindow -PassThru -Wait
if ($browserProcess.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $rendered512)) {
    throw 'Failed to render the canonical SVG icon.'
}

function Write-ScaledPng {
    param(
        [Parameter(Mandatory)] [int] $Size,
        [Parameter(Mandatory)] [string] $Destination
    )

    & $ffmpeg.Source -hide_banner -loglevel error -y -i $rendered512 `
        -vf "scale=${Size}:${Size}:flags=lanczos" -frames:v 1 $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to generate ${Size}x${Size} icon: $Destination"
    }
}

[IO.File]::Copy($sourceSvg, (Join-Path $webPublic 'remoe-icon.svg'), $true)
Write-ScaledPng -Size 32 -Destination (Join-Path $webPublic 'favicon-32.png')
Write-ScaledPng -Size 180 -Destination (Join-Path $webPublic 'apple-touch-icon.png')
Write-ScaledPng -Size 192 -Destination (Join-Path $webPublic 'remoe-icon-192.png')
[IO.File]::Copy($rendered512, (Join-Path $webPublic 'remoe-icon-512.png'), $true)

$icoSizes = @(16, 24, 32, 48, 64, 128, 256)
$icoImages = foreach ($size in $icoSizes) {
    $path = Join-Path $workingDirectory "remoe-${size}.png"
    Write-ScaledPng -Size $size -Destination $path
    [pscustomobject]@{
        Size = $size
        Bytes = [IO.File]::ReadAllBytes($path)
    }
}

$stream = [IO.File]::Open($windowsIcon, [IO.FileMode]::Create, [IO.FileAccess]::Write)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$icoImages.Count)

    $offset = 6 + (16 * $icoImages.Count)
    foreach ($image in $icoImages) {
        $dimension = if ($image.Size -eq 256) { [byte]0 } else { [byte]$image.Size }
        $writer.Write($dimension)
        $writer.Write($dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$image.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $image.Bytes.Length
    }

    foreach ($image in $icoImages) {
        $writer.Write($image.Bytes)
    }
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

[IO.File]::Copy($windowsIcon, (Join-Path $webPublic 'favicon.ico'), $true)

Write-Host 'Generated Web and Windows icons from android-client/design/app-icon.svg.'
