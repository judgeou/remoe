[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$KeystorePath,
    [string]$Alias = 'remoe-release',
    [string]$AssetLinksPath = (Join-Path $PSScriptRoot '../web-client/public/.well-known/assetlinks.json')
)

$ErrorActionPreference = 'Stop'
$resolvedKeystore = (Resolve-Path -LiteralPath $KeystorePath).Path
$temporaryCertificate = Join-Path ([System.IO.Path]::GetTempPath()) ("remoe-cert-{0}.der" -f [guid]::NewGuid())
try {
    & keytool -exportcert -keystore $resolvedKeystore -alias $Alias -file $temporaryCertificate
    if ($LASTEXITCODE -ne 0) { throw "keytool failed with exit code $LASTEXITCODE" }
    $certificateBytes = [System.IO.File]::ReadAllBytes($temporaryCertificate)
    $hash = [System.Security.Cryptography.SHA256]::HashData($certificateBytes)
    $fingerprint = ($hash | ForEach-Object { $_.ToString('X2') }) -join ':'
    $originHash = [Convert]::ToBase64String($hash).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $origin = "android:apk-key-hash:$originHash"

    $statement = [ordered]@{
        relation = @(
            'delegate_permission/common.handle_all_urls'
            'delegate_permission/common.get_login_creds'
        )
        target = [ordered]@{
            namespace = 'android_app'
            package_name = 'top.ozaoza.remoe'
            sha256_cert_fingerprints = @($fingerprint)
        }
    }
    $assetLinks = ConvertTo-Json -InputObject @($statement) -Depth 6
    $assetLinksParent = Split-Path -Parent $AssetLinksPath
    New-Item -ItemType Directory -Force -Path $assetLinksParent | Out-Null
    Set-Content -LiteralPath $AssetLinksPath -Value $assetLinks -Encoding utf8NoBOM

    Write-Host "Certificate SHA-256: $fingerprint"
    Write-Host "Android origin:       $origin"
    Write-Host "assetlinks.json:      $AssetLinksPath"
    Write-Host 'Set REMOE_ANDROID_ORIGINS on the server to the Android origin above.'
} finally {
    if (Test-Path -LiteralPath $temporaryCertificate) {
        Remove-Item -LiteralPath $temporaryCertificate -Force
    }
}
