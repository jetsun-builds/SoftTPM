$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$packageRoot = $PSScriptRoot
function Require-PackagePath([string]$Relative) {
    $full = [IO.Path]::GetFullPath((Join-Path $packageRoot $Relative))
    $prefix = [IO.Path]::GetFullPath($packageRoot).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Path is outside package' }
    if (-not (Test-Path -LiteralPath $full)) { throw "Missing package file: $Relative" }
    return $full
}
function Assert-PackageIntegrity {
    $sums = Require-PackagePath 'SHA256SUMS.txt'
    foreach ($line in Get-Content -LiteralPath $sums) {
        if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Malformed SHA256SUMS.txt' }
        $expected = $Matches[1]
        $file = Require-PackagePath $Matches[2]
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $expected) { throw "File checksum mismatch: $file" }
    }
}
function Initialize-NativeDevice {
    if (-not ('SoftTpmPackage.NativeDevice' -as [type])) { Add-Type -Path (Require-PackagePath 'NativeDevice.cs') }
}
function Invoke-PackageCommand([string]$Command, [string[]]$Arguments, [int[]]$AllowedExitCodes = @(0)) {
    & $Command @Arguments
    if ($LASTEXITCODE -notin $AllowedExitCodes) { throw "$Command failed: exit code $LASTEXITCODE" }
}
function Get-PackageCertificate {
    $manifest = Get-Content -LiteralPath (Require-PackagePath 'build-info.json') -Raw | ConvertFrom-Json
    $cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Require-PackagePath 'SoftTPM-TestSigning.cer'))
    if ($cert.Thumbprint -ine $manifest.certificateThumbprint) { throw 'Certificate differs from build manifest' }
    return $cert
}
