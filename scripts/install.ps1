#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [validateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$DisableExistingTpm,
    [switch]$SkipDriverInstall
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

    foreach ($candidate in @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools\x64\devcon.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools\10.0.22621.0\x64\devcon.exe')
    ))
    {
        if (Test-Path $candidate) { return $candidate }
    }

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

function Test-TpmPresent
{
    if (-not ('SoftTpmInstall.Native' -as [type]))
    {
        Add-Type -Namespace SoftTpmInstall -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(IntPtr hObject);
'@
    }

    # 0xC0000000 = GENERIC_READ | GENERIC_WRITE, 3 = OPEN_EXISTING
    $handle = [SoftTpmInstall.Native]::CreateFileW('\\.\TPM', 3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($handle -eq [IntPtr](-1))
    {
        return (Test-Path '\\.\TPM')
    }

    [SoftTpmInstall.Native]::CloseHandle($handle) | Out-Null
    return $true
}

function Test-TestSigningEnabled
{
    $output = & bcdedit.exe /enum '{current}' 2>$null
    foreach ($line in $output)
    {
        if ($line -match '^\s*testsigning\s+Yes')
        {
            return $true
        }
    }

    return $false
}

if (-not (Test-Administrator))
{
    throw 'install.ps1 must run from an elevated PowerShell session.'
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$softTpmRoot = Split-Path -Parent $scriptDir
$driverPackage = Join-Path $softTpmRoot "driver\x64\$Configuration\SoftTpmDriver"
$driverInf = Join-Path $driverPackage 'SoftTpmDriver.inf'
$serviceExe = Join-Path $softTpmRoot "build\service\$Configuration\SoftTpmService.exe"

$softTpmDevice = @(Get-PnpDevice -PresentOnly -FriendlyName '*SoftTPM*' -ErrorAction SilentlyContinue)
$hardwareTpm = @(Get-PnpDevice -PresentOnly -FriendlyName '*Trusted Platform Module*' -ErrorAction SilentlyContinue)

# A \\.\TPM responder that is our own device node (or our loaded driver
# without a node) is just a previous installation; the upgrade path below
# reinstalls it. Only a present hardware TPM has to be disabled first.
if ((Test-TpmPresent) -and ($softTpmDevice.Count -eq 0))
{
    if ($hardwareTpm.Count -eq 0)
    {
        Write-Host 'A stale SoftTPM responder is active without a device node; reinstalling it.'
    }
    elseif (-not $DisableExistingTpm)
    {
        Write-Warning 'A TPM device already responds at \\.\TPM. The SoftTPM driver claims the same name, so the existing TPM must be disabled first.'
        Write-Warning 'Re-run install.ps1 -DisableExistingTpm to disable it, or disable it manually in Device Manager.'
        throw 'An existing TPM is active.'
    }
    else
    {
        Write-Host 'Disabling the existing TPM device'
        $hardwareTpm | ForEach-Object {
            Write-Host "  Disabling $($_.FriendlyName) ($($_.InstanceId))"
            Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false
        }

        if (Test-TpmPresent)
        {
            throw 'The existing TPM is still active after disabling.'
        }
    }
}

if (-not $SkipDriverInstall)
{
    if (-not (Test-Path $driverInf))
    {
        throw "The driver package was not found at $driverInf. Run build.ps1 first (the packaged INF must sit next to its catalog and driver binary)."
    }

    if (Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue)
    {
        Write-Host 'Stopping the previous SoftTpmService'
        Stop-Service -Name 'SoftTpmService' -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-TestSigningEnabled))
    {
        Write-Warning 'Test signing is not enabled. The driver will fail to load unless it carries a Microsoft signature.'
        Write-Warning 'Enable it with: bcdedit /set testsigning on  (Secure Boot must also be disabled), then reboot.'
    }

    $devcon = Find-DevCon
    if (-not $devcon)
    {
        throw 'devcon.exe was not found. Install the WDK (which includes devcon) or install the driver manually with "devcon install SoftTpmDriver.inf Root\SoftTpm".'
    }

    if ($softTpmDevice.Count -gt 0)
    {
        Write-Host 'Removing the existing SoftTPM device node so the previous driver can unload'
        Invoke-External $devcon remove 'Root\SoftTpm'
        Start-Sleep -Milliseconds 500
    }

    # Remove stale copies of this package from the DriverStore. Otherwise
    # pnputil reports "already exists" and devcon relinks the old binary from
    # the DriverStore back into System32, silently undoing our swap.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $enumOutput = & pnputil.exe /enum-drivers 2>&1
    $ErrorActionPreference = $previous

    $publishedName = $null
    $stalePackages = @()
    foreach ($line in $enumOutput)
    {
        $text = [string]$line
        if ($text -match '^\s*Published Name\s*:\s*(\S+)')
        {
            $publishedName = $Matches[1]
        }
        elseif ($text -match '^\s*Original Name\s*:\s*(\S+)')
        {
            if ($publishedName -and $Matches[1] -ieq 'softtpmdriver.inf')
            {
                $stalePackages += $publishedName
            }
            $publishedName = $null
        }
    }

    foreach ($oem in $stalePackages)
    {
        Write-Host "Removing stale DriverStore package $oem"
        Invoke-External pnputil.exe /delete-driver $oem /uninstall /force
        if ($script:ExternalExitCode -ne 0)
        {
            Write-Warning "Could not remove $oem (exit code $script:ExternalExitCode); continuing."
        }
    }

    Write-Host 'Staging the driver package'
    Invoke-External pnputil.exe /add-driver $driverInf /install

    # 259 (ERROR_NO_MORE_ITEMS) = package already present and device up to date
    if ($script:ExternalExitCode -ne 0 -and $script:ExternalExitCode -ne 3010 -and $script:ExternalExitCode -ne 259)
    {
        throw 'pnputil failed to add the driver package.'
    }
    if ($script:ExternalExitCode -eq 3010)
    {
        Write-Warning 'pnputil reports a pending reboot.'
    }

    Write-Host 'Creating the SoftTPM device node'
    Invoke-External $devcon install $driverInf 'Root\SoftTpm'
    if ($script:ExternalExitCode -ne 0)
    {
        Write-Warning 'devcon install failed; updating the existing device instead'
        Invoke-External $devcon update $driverInf 'Root\SoftTpm'
    }
    if ($script:ExternalExitCode -ne 0)
    {
        throw 'devcon failed to install the device.'
    }

    # The installed binary must be exactly the freshly built package. This runs
    # after pnputil/devcon because the PnP path can relink an old DriverStore
    # copy over System32 without reporting any error.
    $packageSys = Join-Path $driverPackage 'SoftTpmDriver.sys'
    $installedSys = Join-Path $env:SystemRoot 'System32\drivers\SoftTpmDriver.sys'
    if (Test-Path $packageSys)
    {
        $packageHash = (Get-FileHash -Path $packageSys -Algorithm MD5).Hash
        $swapped = -not (Test-Path $installedSys)
        if (-not $swapped)
        {
            try
            {
                Remove-Item $installedSys -Force -ErrorAction Stop
                $swapped = $true
            }
            catch
            {
                $swapped = $false
            }
        }

        if ($swapped)
        {
            Copy-Item $packageSys $installedSys -Force
            $installedHash = (Get-FileHash -Path $installedSys -Algorithm MD5).Hash
            if ($installedHash -ne $packageHash)
            {
                throw "The installed driver file does not match the package (expected $packageHash, got $installedHash)."
            }
            Write-Host "Replaced the installed driver file directly (md5 $installedHash)"
        }
        else
        {
            # Still pinned by a loaded image; stage the package file for the
            # next boot. The staged copy must live on a local disk (Z: is not
            # available when pending renames run).
            $stagedSys = Join-Path $env:ProgramFiles 'SoftTpm\SoftTpmDriver.sys'
            New-Item -ItemType Directory -Force (Split-Path -Parent $stagedSys) | Out-Null
            Copy-Item $packageSys $stagedSys -Force

            $sessionManager = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
            $pending = @((Get-ItemProperty -Path $sessionManager -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations)
            # Drop leftover SoftTpmDriver entries so a stale pair cannot fire
            # at the next boot and overwrite the new binary.
            $pending = @($pending | Where-Object { $_ -and ($_ -notmatch 'SoftTpmDriver\.sys') })
            $pending += ('\??\' + $stagedSys)
            $pending += ('!\??\' + $installedSys)
            Set-ItemProperty -Path $sessionManager -Name PendingFileRenameOperations -Value $pending -Type MultiString

            Write-Warning 'System32\drivers\SoftTpmDriver.sys is still held by the loaded driver image.'
            Write-Warning "A replacement (md5 $packageHash) is scheduled for the next boot. Reboot first, then run the trace step; do not trace before rebooting."
        }
    }

    Write-Host 'Waiting for \\.\TPM to appear'
    for ($attempt = 0; $attempt -lt 20 -and -not (Test-TpmPresent); $attempt++)
    {
        Start-Sleep -Milliseconds 500
    }

    if (-not (Test-TpmPresent))
    {
        Write-Warning 'The driver is loaded but \\.\TPM did not appear. Check the trace with IOCTL_SOFTTPM_READ_TRACE or Device Manager.'
    }
}

if (-not (Test-Path $serviceExe))
{
    Write-Warning "SoftTpmService.exe was not found at $serviceExe; skipping service registration."
    return
}

$serviceSourceDir = Split-Path -Parent $serviceExe
$serviceInstallDir = Join-Path $env:ProgramFiles 'SoftTpm'
$serviceInstalledExe = Join-Path $serviceInstallDir 'SoftTpmService.exe'

$existingService = Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue
if ($existingService)
{
    Write-Host 'Removing the previous service registration'
    Stop-Service -Name 'SoftTpmService' -Force -ErrorAction SilentlyContinue
    Invoke-External sc.exe delete SoftTpmService

    for ($attempt = 0; $attempt -lt 10 -and (Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue); $attempt++)
    {
        Start-Sleep -Milliseconds 500
    }
}

Write-Host "Copying the service payload to $serviceInstallDir"
New-Item -ItemType Directory -Force $serviceInstallDir | Out-Null
Copy-Item (Join-Path $serviceSourceDir '*') $serviceInstallDir -Recurse -Force

Write-Host 'Registering the SoftTPM service'
Invoke-External sc.exe create SoftTpmService binPath= "`"$serviceInstalledExe`"" start= auto DisplayName= "`"SoftTPM Software TPM 2.0`""
if ($script:ExternalExitCode -ne 0)
{
    throw 'sc.exe failed to create the service.'
}

Invoke-External sc.exe description SoftTpmService "`"Hosts the SoftTPM TPM 2.0 emulator engine and serves commands to the SoftTpm driver.`""
Invoke-External sc.exe start SoftTpmService
if ($script:ExternalExitCode -ne 0)
{
    throw 'The service failed to start. Check %ProgramData%\SoftTpm\softtpm.log.'
}

Write-Host ''
Write-Host 'Installed. Verify with:'
Write-Host "  $softTpmRoot\tools\SoftTpmStatus\bin\SoftTpmStatus.exe"
Write-Host '  Get-Tpm'
