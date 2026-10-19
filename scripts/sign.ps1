#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string]$CertificateName = 'SoftTpmTest',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$script:ExternalExitCode = 0

function Invoke-External
{
    param()

    $FilePath = $args[0]
    $remaining = @()
    if ($args.Count -gt 1)
    {
        $remaining = $args[1..($args.Count - 1)]
    }

    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try
    {
        & $FilePath @remaining
        $script:ExternalExitCode = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
}

function Find-SignTool
{
    $command = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path $kitsRoot)
    {
        $found = Get-ChildItem $kitsRoot -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($found) { return $found }
    }

    throw 'signtool.exe was not found. Install the Windows SDK.'
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$softTpmRoot = Split-Path -Parent $scriptDir

$packageDir = Join-Path $softTpmRoot "driver\x64\$Configuration\SoftTpmDriver"
$catalogPaths = @()
if (Test-Path $packageDir)
{
    $catalogPaths = @(Get-ChildItem (Join-Path $packageDir '*.cat') -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
}

$binaryPaths = @(
    (Join-Path $softTpmRoot "driver\x64\$Configuration\SoftTpmDriver.sys"),
    (Join-Path $softTpmRoot "build\service\$Configuration\SoftTpmService.exe"),
    (Join-Path $softTpmRoot 'tools\SoftTpmStatus\bin\SoftTpmStatus.exe')
) + $catalogPaths

Write-Host "Looking for the $CertificateName code signing certificate"

$certificate = Get-ChildItem 'Cert:\CurrentUser\My' |
    Where-Object { $_.Subject -eq "CN=$CertificateName" } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate)
{
    Write-Host 'Creating a self-signed code signing certificate'
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=$CertificateName" `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears(5) `
        -KeyUsage DigitalSignature `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
}

Write-Host "Certificate thumbprint: $($certificate.Thumbprint)"

foreach ($store in @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher'))
{
    $existing = Get-ChildItem $store | Where-Object { $_.Thumbprint -eq $certificate.Thumbprint }
    if (-not $existing)
    {
        Write-Host "Trusting the certificate in $store (requires administrator)"
        $temporary = New-TemporaryFile
        Export-Certificate -Cert $certificate -FilePath $temporary.FullName | Out-Null
        Import-Certificate -FilePath $temporary.FullName -CertStoreLocation $store | Out-Null
        Remove-Item $temporary.FullName -Force
    }
}

$signtool = Find-SignTool

$wdkCertificatePath = Join-Path $softTpmRoot "driver\x64\$Configuration\SoftTpmDriver.cer"
if (Test-Path $wdkCertificatePath)
{
    $wdkCertificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($wdkCertificatePath)
    foreach ($store in @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher'))
    {
        $existing = Get-ChildItem $store | Where-Object { $_.Thumbprint -eq $wdkCertificate.Thumbprint }
        if (-not $existing)
        {
            Write-Host "Trusting the WDK build certificate $($wdkCertificate.Thumbprint) in $store"
            Import-Certificate -FilePath $wdkCertificatePath -CertStoreLocation $store | Out-Null
        }
    }
}

$signedAny = $false

foreach ($binary in $binaryPaths)
{
    if (-not (Test-Path $binary))
    {
        Write-Warning "Not found, skipping: $binary"
        continue
    }

    Write-Host "Signing $binary"
    Invoke-External $signtool sign /fd SHA256 /sha1 $certificate.Thumbprint /s My $binary

    if ($script:ExternalExitCode -ne 0)
    {
        throw "Signing failed for $binary"
    }

    $signedAny = $true
}

if (-not $signedAny)
{
    throw 'No binaries were signed. Run build.ps1 first.'
}

Write-Host ''
Write-Host 'Signed successfully.'
Write-Host 'Test signing must be enabled for the driver to load:  bcdedit /set testsigning on'
