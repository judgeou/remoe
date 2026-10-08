[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$sourceDirectory = $PSScriptRoot
$buildDirectory = Join-Path $sourceDirectory 'build-recorder'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer's vswhere.exe was not found. Install Visual Studio with the Desktop development with C++ workload."
}

$visualStudio = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $visualStudio) {
    throw 'No Visual Studio installation with the x64 C++ toolchain was found.'
}

$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "MSVC environment script was not found: $vcvars"
}

# vcvars64.bat prepends many directories to PATH. Starting it from an already
# initialized developer shell can exceed cmd.exe's 8,191-character line limit,
# so give it a small, deterministic Windows PATH and restore the caller's
# process environment when the build finishes.
$originalEnvironment = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $originalEnvironment[[string]$entry.Key] = [string]$entry.Value
}

try {
    $env:PATH = @(
        (Join-Path $env:SystemRoot 'System32')
        $env:SystemRoot
        (Join-Path $env:SystemRoot 'System32\Wbem')
        (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0')
    ) -join ';'

    # Import the environment produced by vcvars64.bat into this process.
    $vcvarsCommand = 'call "{0}" >nul && set' -f $vcvars
    $environmentLines = & cmd.exe /d /s /c $vcvarsCommand
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }

foreach ($command in 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command was not found in PATH. Install the Visual Studio C++ CMake tools component."
    }
}

if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $buildDirectory).Path
    $resolvedSource = (Resolve-Path -LiteralPath $sourceDirectory).Path
    $expectedPrefix = $resolvedSource + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedBuild.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside the project: $resolvedBuild"
    }
    Write-Host "Removing $resolvedBuild"
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

Write-Host 'Configuring remoe (Ninja Multi-Config, x64)...'
& cmake.exe -S $sourceDirectory -B $buildDirectory -G 'Ninja Multi-Config'
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

Write-Host "Building $Configuration..."
& cmake.exe --build $buildDirectory --config $Configuration --target remoe_recorder --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$outputDirectory = Join-Path $buildDirectory $Configuration
$artifacts = @('remoe_recorder.exe') | ForEach-Object {
    Join-Path $outputDirectory $_
} | Where-Object {
    Test-Path -LiteralPath $_
}

if ($artifacts.Count -eq 0) {
    throw "Build completed but no remoe executable was found in $outputDirectory."
}

Write-Host 'Build succeeded:'
foreach ($artifact in $artifacts) {
    $file = Get-Item -LiteralPath $artifact
    Write-Host ("  {0} ({1:N0} bytes)" -f $file.FullName, $file.Length)
}
}
finally {
    foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if (-not $originalEnvironment.ContainsKey([string]$name)) {
            [Environment]::SetEnvironmentVariable([string]$name, $null, 'Process')
        }
    }
    foreach ($entry in $originalEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
}

