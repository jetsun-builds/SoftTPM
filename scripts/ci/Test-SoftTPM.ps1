#Requires -Version 5.1
#Requires -RunAsAdministrator
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$common = Join-Path $PSScriptRoot 'Package-Common.ps1'
if (-not (Test-Path -LiteralPath $common)) { throw 'Extract the complete package first' }
. $common
Assert-PackageIntegrity
Initialize-NativeDevice
if (-not [SoftTpmPackage.NativeDevice]::Exists()) { throw 'SoftTPM device is not installed' }
$service = Get-Service -Name SoftTpmService -ErrorAction Stop
if ($service.Status -ne 'Running') { Start-Service -Name SoftTpmService -ErrorAction Stop }
$ready = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    $tpm = Get-Tpm -ErrorAction Stop
    if ($tpm -and $tpm.TpmPresent -and $tpm.TpmReady) { $ready = $true; break }
    Start-Sleep -Seconds 2
}
Get-Tpm | Format-List
if (-not $ready) { throw 'TPM did not become ready within 60 seconds. Inspect %ProgramData%/SoftTpm/softtpm.log; do not clear TPM state.' }
Invoke-PackageCommand tpmtool.exe @('getdeviceinformation')
Invoke-PackageCommand (Require-PackagePath 'tools/SoftTpmStatus.exe') @()
Write-Host 'Runtime TPM checks passed. Windows Update eligibility still needs separate verification.'
