#Requires -RunAsAdministrator

<#
.SYNOPSIS
Captures the firmware TCG event log (WBCL) into a file that SoftTPM replays.

.DESCRIPTION
Boot the machine with a real firmware TPM (or a firmware vTPM) enabled, run
this script before installing SoftTPM, then copy the captured file to
%ProgramData%\SoftTpm\platform-log.bin on the SoftTPM system (or pass it with
--platform-log=PATH). On the next service start SoftTPM replays the events
into its PCRs and publishes the log as the WBCL.

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Capture -OutputPath C:\temp\platform-log.bin
#>

[CmdletBinding()]
param(
    [string]$OutputPath = (Join-Path $env:ProgramData 'SoftTpm\platform-log.bin')
)

$ErrorActionPreference = 'Stop'

$wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction SilentlyContinue).'WBCL'

if (-not $wbcl)
{
    throw 'No WBCL registry value found. Boot with the firmware TPM (or firmware vTPM) enabled and run this before installing SoftTPM.'
}

$headerBytes = @(0, 0, 0, 0, 3, 0, 0, 0)
$hasTcgHeader = $true
for ($i = 0; $i -lt $headerBytes.Count; $i++)
{
    if ($wbcl[$i] -ne $headerBytes[$i])
    {
        $hasTcgHeader = $false
        break
    }
}

if (-not $hasTcgHeader)
{
    throw 'The WBCL value does not start with a TCG log header (PCR 0, EV_NO_ACTION); refusing to capture it.'
}

if (Get-Service -Name SoftTpmService -ErrorAction SilentlyContinue)
{
    Write-Warning 'SoftTpmService is installed; if the firmware TPM was disabled, this captures SoftTPM''s own published log, not a firmware log. Remove SoftTPM, re-enable the firmware TPM and capture again.'
}

if ($wbcl.Length -lt 128)
{
    Write-Warning "The captured log is only $($wbcl.Length) bytes and may contain no firmware measurements."
}

$directory = Split-Path -Parent $OutputPath
if ($directory -and -not (Test-Path $directory))
{
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

[System.IO.File]::WriteAllBytes($OutputPath, $wbcl)

"captured $($wbcl.Length) bytes to $OutputPath"
