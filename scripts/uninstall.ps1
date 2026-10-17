#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [switch]$RemoveState,
    [switch]$EnableExistingTpm
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

function Test-Administrator
{
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-DevCon
{
    $command = Get-Command 'devcon.exe' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $kitsTools = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
    if (Test-Path $kitsTools)
    {
        $found = Get-ChildItem $kitsTools -Recurse -Filter 'devcon.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'x64' } |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }

    return $null
}

if (-not (Test-Administrator))
{
    throw 'uninstall.ps1 must run from an elevated PowerShell session.'
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$softTpmRoot = Split-Path -Parent $scriptDir
$stateDirectory = Join-Path $env:ProgramData 'SoftTpm'

$service = Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue
if ($service)
{
    Write-Host 'Stopping the SoftTPM service'
    Stop-Service -Name 'SoftTpmService' -Force -ErrorAction SilentlyContinue
    Invoke-External sc.exe delete SoftTpmService
}
else
{
    Write-Host 'The SoftTPM service is not registered.'
}

$serviceInstallDir = Join-Path $env:ProgramFiles 'SoftTpm'
if (Test-Path $serviceInstallDir)
{
    Write-Host "Removing the service payload at $serviceInstallDir"
    Remove-Item $serviceInstallDir -Recurse -Force -ErrorAction SilentlyContinue
}

if (Test-Path '\\.\TPM')
{
    $devcon = Find-DevCon

    if ($devcon)
    {
        Write-Host 'Removing the SoftTPM device node'
        Invoke-External $devcon remove 'Root\SoftTpm'
        if ($script:ExternalExitCode -ne 0)
        {
            Write-Warning 'devcon remove reported a failure; the device may already be gone.'
        }

        foreach ($configuration in @('Release', 'Debug'))
        {
            $inf = Join-Path $softTpmRoot "driver\x64\$configuration\SoftTpmDriver.inf"
            if (Test-Path $inf)
            {
                Invoke-External $devcon remove $inf
            }
        }
    }
    else
    {
        Write-Warning 'devcon.exe was not found; remove the device manually in Device Manager (SoftTPM under Security devices).'
    }
}

$publishedNames = @()

try
{
    $publishedNames = @(
        Get-WindowsDriver -Online -All |
            Where-Object { $_.OriginalFileName -and $_.OriginalFileName -imatch 'softtpmdriver\.inf$' } |
            Select-Object -ExpandProperty Driver
    )
}
catch
{
    Write-Warning "Could not enumerate the driver store: $($_.Exception.Message)"
}

foreach ($publishedName in $publishedNames)
{
    Write-Host "Removing the staged driver package $publishedName"
    Invoke-External pnputil.exe /delete-driver $publishedName /uninstall /force
}

if ($publishedNames.Count -eq 0)
{
    Write-Host 'No staged SoftTPM driver package was found.'
}

if ($RemoveState)
{
    if (Test-Path $stateDirectory)
    {
        Write-Host "Deleting TPM state at $stateDirectory"
        Remove-Item $stateDirectory -Recurse -Force
        Write-Host 'The TPM identity, persistent keys and EK certificate were destroyed.'
        Write-Host 'BitLocker volumes protected by SoftTPM will need their recovery keys.'
    }
    else
    {
        Write-Host 'There is no TPM state to delete.'
    }
}

if ($EnableExistingTpm)
{
    Write-Host 'Re-enabling the previous TPM device'
    Get-PnpDevice | Where-Object { $_.FriendlyName -like '*Trusted Platform Module*' } | ForEach-Object {
        Write-Host "  Enabling $($_.FriendlyName) ($($_.InstanceId))"
        Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}

Write-Host ''
Write-Host 'Uninstall complete. A reboot is recommended.'
