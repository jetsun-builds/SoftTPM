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
$service = Get-Service -Name SoftTpmService -ErrorAction SilentlyContinue
if ($service) {
    Stop-Service -Name SoftTpmService -ErrorAction Stop
    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
    Invoke-PackageCommand sc.exe @('delete','SoftTpmService')
}
Write-Host ('Removed SoftTPM device nodes: ' + [SoftTpmPackage.NativeDevice]::RemoveAll())
$drivers = @(Get-WindowsDriver -Online -All -ErrorAction Stop | Where-Object { $_.OriginalFileName -match '(?i)(^|[\\/])softtpmdriver\.inf$' -and $_.ProviderName -eq 'SoftTPM Project' })
foreach ($driver in $drivers) {
    if ($driver.Driver -notmatch '^oem\d+\.inf$') { throw 'Unexpected published driver name' }
    Invoke-PackageCommand pnputil.exe @('/delete-driver',$driver.Driver,'/uninstall') @(0,3010)
}
$installDir = Join-Path $env:ProgramFiles 'SoftTpm'
if (Test-Path -LiteralPath $installDir) {
    $resolved = (Resolve-Path -LiteralPath $installDir).Path
    $expected = [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'SoftTpm'))
    if ($resolved -ine $expected) { throw 'Unexpected installation directory; refusing recursive delete' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
$cert = Get-PackageCertificate
foreach ($store in @('Cert:/LocalMachine/Root','Cert:/LocalMachine/TrustedPublisher')) {
    $certPath = "$store/$($cert.Thumbprint)"
    if (Test-Path -LiteralPath $certPath) { Remove-Item -LiteralPath $certPath }
}
Write-Host 'Uninstalled. TPM state in %ProgramData%/SoftTpm was preserved. Reboot before turning test signing off or re-enabling Secure Boot.'
