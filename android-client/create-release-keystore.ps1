[CmdletBinding()]
param(
    [string]$OutputPath = (Join-Path $PSScriptRoot 'private/remoe-release.p12'),
    [string]$Alias = 'remoe-release'
)

$ErrorActionPreference = 'Stop'
$resolvedParent = [System.IO.Path]::GetFullPath((Split-Path -Parent $OutputPath))
$privateRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'private'))
$privatePrefix = $privateRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar
if ($resolvedParent -ne $privateRoot -and
    -not $resolvedParent.StartsWith($privatePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must stay inside $privateRoot"
}
if (Test-Path -LiteralPath $OutputPath) {
    throw "Refusing to overwrite existing keystore: $OutputPath"
}

New-Item -ItemType Directory -Force -Path $resolvedParent | Out-Null
Write-Host 'keytool will now ask for the new keystore password. Store it in a password manager.'
Write-Host 'Keep at least two offline backups; losing this key prevents signed updates to existing installs.'
& keytool -genkeypair `
    -keystore $OutputPath `
    -storetype PKCS12 `
    -alias $Alias `
    -keyalg RSA `
    -keysize 4096 `
    -validity 10000 `
    -dname 'CN=remoe Android, O=remoe'
if ($LASTEXITCODE -ne 0) { throw "keytool failed with exit code $LASTEXITCODE" }
Write-Host "Created: $OutputPath"
Write-Host "Alias:   $Alias"
