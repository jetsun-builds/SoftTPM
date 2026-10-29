#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$common = Join-Path $PSScriptRoot 'Package-Common.ps1'
if (-not (Test-Path -LiteralPath $common)) { throw 'Extract the complete package first' }
. $common
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit Windows PowerShell' }
if ([Environment]::OSVersion.Version.Build -lt 26100) { throw 'This package targets Windows 11 24H2 or later' }
Assert-PackageIntegrity
Initialize-NativeDevice
if (-not [SoftTpmPackage.NativeDevice]::TestSigningEnabled()) {
    throw 'Test signing is not active. Disable Secure Boot in BIOS, run bcdedit /set testsigning on as administrator, reboot, then run this script again.'
}
$existingTpm = @(Get-CimInstance -Namespace 'root/CIMV2/Security/MicrosoftTpm' -ClassName Win32_Tpm -ErrorAction Stop)
if ($existingTpm.Count -gt 0 -and -not [SoftTpmPackage.NativeDevice]::Exists()) {
    throw 'An existing TPM is present. This package will not disable or replace a hardware/firmware TPM.'
}
$cert = Get-PackageCertificate
foreach ($binary in @('driver/SoftTpmDriver.sys','service/SoftTpmService.exe','tools/SoftTpmStatus.exe')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Require-PackagePath $binary)
    if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ine $cert.Thumbprint) { throw "Unexpected signer: $binary" }
}
$certificateFile = Require-PackagePath 'SoftTPM-TestSigning.cer'
foreach ($store in @('Cert:/LocalMachine/Root','Cert:/LocalMachine/TrustedPublisher')) {
    if (-not (Test-Path -LiteralPath $store)) { throw "Certificate store missing: $store" }
    Import-Certificate -FilePath $certificateFile -CertStoreLocation $store | Out-Null
}
foreach ($binary in @('driver/SoftTpmDriver.sys','driver/SoftTpmDriver.cat')) {
    if ((Get-AuthenticodeSignature -LiteralPath (Require-PackagePath $binary)).Status -ne 'Valid') { throw "Signature verification failed: $binary" }
}
$service = Get-Service -Name SoftTpmService -ErrorAction SilentlyContinue
if ($service) { Stop-Service -Name SoftTpmService -ErrorAction Stop; $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20)) }
$installDir = Join-Path $env:ProgramFiles 'SoftTpm'
if (-not (Test-Path -LiteralPath $installDir)) { New-Item -ItemType Directory -Path $installDir | Out-Null }
foreach ($file in Get-ChildItem -LiteralPath (Require-PackagePath 'service') -File) { Copy-Item -LiteralPath $file.FullName -Destination $installDir -Force }
foreach ($file in @('SoftTPM-TestSigning.cer','build-info.json')) { Copy-Item -LiteralPath (Require-PackagePath $file) -Destination $installDir -Force }
$inf = Require-PackagePath 'driver/SoftTpmDriver.inf'
Invoke-PackageCommand pnputil.exe @('/add-driver',$inf,'/install') @(0,259,3010)
$rebootRequired = [SoftTpmPackage.NativeDevice]::Install($inf)
$installedExe = Join-Path $installDir 'SoftTpmService.exe'
if (-not (Test-Path -LiteralPath $installedExe)) { throw 'Installed service file missing' }
if ($service) {
    $serviceCim = Get-CimInstance Win32_Service -Filter "Name='SoftTpmService'"
    $changed = Invoke-CimMethod -InputObject $serviceCim -MethodName Change -Arguments @{ PathName=('"{0}"' -f $installedExe); StartMode='Automatic' }
    if ($changed.ReturnValue -ne 0) { throw "Service configuration failed: $($changed.ReturnValue)" }
} else {
    New-Service -Name SoftTpmService -BinaryPathName ('"{0}"' -f $installedExe) -DisplayName 'SoftTPM Software TPM 2.0' -StartupType Automatic | Out-Null
}
if ($rebootRequired) { Write-Warning 'Driver installation requires a reboot. Reboot, then run Test-SoftTPM.ps1.'; return }
try { Start-Service -Name SoftTpmService -ErrorAction Stop } catch { throw "Service start failed. Reboot and retry Test-SoftTPM.ps1; inspect %ProgramData%/SoftTpm/softtpm.log. $($_.Exception.Message)" }
Write-Host 'Installed. Wait for provisioning, then run Test-SoftTPM.ps1. No restart was forced.'
