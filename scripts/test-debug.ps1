#Requires -RunAsAdministrator

<#
.SYNOPSIS
Runs the SoftTPM end-to-end test and diagnostics helpers.

.DESCRIPTION
This is the single entry point for everything that is not build, sign,
install or uninstall. The implementation lives under scripts\lib so the
top-level scripts stay small and predictable.

Modes:
  Full      Rebuild the service, refresh the installation, run provisioning
            and the platform-log replay checks, collect logs (default).
  Capture   Capture the firmware WBCL from the registry into a file that
            SoftTPM can replay.
  Inspect   Validate a captured TCG2 log offline and explain every event.
  Validate  Verify the first boot after SoftTPM published a replayed log.
  Trace     Dump service/device state, the driver trace ring and a direct
            driver round trip (bypassing TBS).
  Status    Print a quick snapshot of the TPM stack, WBCL and platform log.
  Session   Dot-source interactive helpers ($S, $L, Invoke-Step) into the
            current PowerShell window.

.EXAMPLE
.\scripts\test-debug.ps1

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Full -ReinstallDriver -AttestationTest

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Inspect -Path C:\temp\platform-log.bin

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Validate

.EXAMPLE
. .\scripts\test-debug.ps1 -Mode Session
#>

[CmdletBinding()]
param(
    [ValidateSet('Full', 'Capture', 'Inspect', 'Validate', 'Trace', 'Status', 'Session')]
    [string]$Mode = 'Full',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$OpenSslRoot = $env:SOFTTPM_OPENSSL_ROOT,

    [switch]$SkipBuild,
    [switch]$SkipCdb,
    [switch]$ConsoleDebug,
    [switch]$ReinstallDriver,
    [switch]$AttestationTest,

    [string]$OutputPath = (Join-Path $env:ProgramData 'SoftTpm\platform-log.bin'),
    [string[]]$Path,
    [string]$PlatformLog = (Join-Path $env:ProgramData 'SoftTpm\platform-log.bin'),
    [string]$ServiceLog = (Join-Path $env:ProgramData 'SoftTpm\softtpm.log'),
    [switch]$SkipGatherLogs
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$softTpmRoot = Split-Path -Parent $scriptDir
$libDir = Join-Path $scriptDir 'lib'
$logDirectory = Join-Path $softTpmRoot 'logs'
$stateDirectory = Join-Path $env:ProgramData 'SoftTpm'

function Show-Status
{
    $service = Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue
    '== service =='
    if ($service)
    {
        $service | Format-List Name,Status,StartType | Out-String
    }
    else
    {
        'SoftTpmService is not installed'
    }

    '== device =='
    if (Test-Path '\\.\TPM')
    {
        'the TPM device responds at \\.\TPM'
    }
    else
    {
        'the TPM device is not present'
    }

    Get-PnpDevice -PresentOnly -FriendlyName '*SoftTPM*' -ErrorAction SilentlyContinue |
        Format-List FriendlyName,Status,Class,InstanceId,Problem | Out-String

    '== TPM stack =='
    try { Get-Tpm | Format-List | Out-String } catch { "Get-Tpm failed: $($_.Exception.Message)" }

    $statusTool = Join-Path $softTpmRoot 'tools\SoftTpmStatus\bin\SoftTpmStatus.exe'
    if (Test-Path $statusTool)
    {
        '== SoftTpmStatus =='
        try { & $statusTool 2>&1 | Out-String } catch { "SoftTpmStatus failed: $($_.Exception.Message)" }
    }
    else
    {
        "SoftTpmStatus was not found at $statusTool"
    }

    '== tpmtool =='
    try { & tpmtool.exe getdeviceinformation 2>&1 | Out-String } catch { "tpmtool failed: $($_.Exception.Message)" }

    '== platform log =='
    $activeLog = Join-Path $stateDirectory 'platform-log.bin'
    if (Test-Path $activeLog)
    {
        "platform log: $activeLog ($((Get-Item $activeLog).Length) bytes)"
    }
    else
    {
        'platform log: not installed'
    }

    try
    {
        $wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction Stop).'WBCL'
        "WBCL registry value: $($wbcl.Length) bytes"
    }
    catch
    {
        'WBCL registry value: not present'
    }

    '== recent service log =='
    $serviceLog = Join-Path $stateDirectory 'softtpm.log'
    if (Test-Path $serviceLog)
    {
        Get-Content $serviceLog -Tail 20 | Out-String
    }
    else
    {
        "service log not found at $serviceLog"
    }
}

function Initialize-Session
{
    $global:S = $softTpmRoot
    $global:L = $logDirectory
    New-Item -ItemType Directory -Force $global:L | Out-Null
    Set-Location $global:S

    function global:Invoke-Step([string]$name, [scriptblock]$body)
    {
        $ok = $true
        try { & $body 2>&1 | Tee-Object -FilePath "$global:L\$name.log" } catch {
            $ok = $false
            "ERROR: $($_.Exception.Message)" | Tee-Object -FilePath "$global:L\$name.log" -Append
            $_.ScriptStackTrace | Tee-Object -FilePath "$global:L\$name.log" -Append
        }
        "result: $ok (exit code: $LASTEXITCODE)" | Add-Content "$global:L\$name.log"
    }

    Write-Host "SoftTPM session ready ($global:S)"
}

switch ($Mode)
{
    'Full'
    {
        $arguments = @{
            Configuration = $Configuration
            SkipBuild = $SkipBuild
            SkipCdb = $SkipCdb
            ConsoleDebug = $ConsoleDebug
            ReinstallDriver = $ReinstallDriver
            AttestationTest = $AttestationTest
        }
        if ($OpenSslRoot) { $arguments.OpenSslRoot = $OpenSslRoot }
        $global:LASTEXITCODE = 0
        & (Join-Path $libDir 'run-provision-test.ps1') @arguments
    }
    'Capture'
    {
        $global:LASTEXITCODE = 0
        & (Join-Path $libDir 'capture-platform-log.ps1') -OutputPath $OutputPath
    }
    'Inspect'
    {
        $global:LASTEXITCODE = 0
        if ($Path)
        {
            & (Join-Path $libDir 'inspect-boot-log.ps1') -Path $Path
        }
        else
        {
            & (Join-Path $libDir 'inspect-boot-log.ps1')
        }
    }
    'Validate'
    {
        $arguments = @{
            PlatformLog = $PlatformLog
            ServiceLog = $ServiceLog
            SkipGatherLogs = $SkipGatherLogs
        }
        $global:LASTEXITCODE = 0
        & (Join-Path $libDir 'validate-reboot-replay.ps1') @arguments
    }
    'Trace'
    {
        $global:LASTEXITCODE = 0
        & (Join-Path $libDir 'trace.ps1')
    }
    'Status'
    {
        Show-Status
    }
    'Session'
    {
        Initialize-Session
    }
}

if ($Mode -eq 'Session')
{
    return
}

if ($Mode -eq 'Status')
{
    exit 0
}

exit $LASTEXITCODE
