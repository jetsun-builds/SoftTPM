#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string]$OpenSslRoot = 'C:\Program Files\OpenSSL-Win64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$SkipCdb,
    [switch]$ConsoleDebug,
    [switch]$ReinstallDriver,
    [switch]$AttestationTest
)

$ErrorActionPreference = 'Stop'

$SoftTpmRoot = Split-Path -Parent $PSScriptRoot
$LogDirectory = Join-Path $SoftTpmRoot 'logs'
$StateDirectory = Join-Path $env:ProgramData 'SoftTpm'
$ServiceLog = Join-Path $StateDirectory 'softtpm.log'
$DumpDirectory = Join-Path $StateDirectory 'dumps'
$Debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'

New-Item -ItemType Directory -Force $LogDirectory | Out-Null
Set-Location $SoftTpmRoot

function Write-Step([string]$Message)
{
    Write-Output ''
    Write-Output "==> $Message"
}

function Invoke-Step([string]$name, [scriptblock]$body)
{
    $ok = $true
    $global:LASTEXITCODE = 0
    try { & $body 2>&1 | Tee-Object -FilePath "$LogDirectory\$name.log" } catch {
        $ok = $false
        "ERROR: $($_.Exception.Message)" | Tee-Object -FilePath "$LogDirectory\$name.log" -Append
        $_.ScriptStackTrace | Tee-Object -FilePath "$LogDirectory\$name.log" -Append
    }
    "result: $ok (exit code: $LASTEXITCODE)" | Add-Content "$LogDirectory\$name.log"
}

Invoke-Step 'prepare-dumps' {
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\SoftTpmService.exe'
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpFolder -Value $DumpDirectory -PropertyType ExpandString -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpType -Value 2 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpCount -Value 5 -PropertyType DWord -Force | Out-Null
    New-Item -ItemType Directory -Force $DumpDirectory | Out-Null
    "crash dumps will be written to $DumpDirectory"
}

Invoke-Step 'enable-wer' {
    try
    {
        Set-Service WerSvc -StartupType Automatic
        Start-Service WerSvc
    }
    catch
    {
        "WerSvc setup failed: $($_.Exception.Message)"
    }

    Get-Service WerSvc -ErrorAction SilentlyContinue |
        Format-List Name,Status,StartType | Out-String
}

if (-not $SkipBuild)
{
    Invoke-Step 'build-full' {
        if ($ReinstallDriver)
        {
            & "$SoftTpmRoot\scripts\build.ps1" -OpenSslRoot $OpenSslRoot -Configuration $Configuration
        }
        else
        {
            & "$SoftTpmRoot\scripts\build.ps1" -SkipDriver -OpenSslRoot $OpenSslRoot -Configuration $Configuration
        }
    }
}

Invoke-Step 'install-service' {
    if ($ReinstallDriver)
    {
        & "$SoftTpmRoot\scripts\install.ps1" -Configuration $Configuration
    }
    else
    {
        & "$SoftTpmRoot\scripts\install.ps1" -SkipDriverInstall -Configuration $Configuration
    }
}

$driverStagedForReboot = @(
    (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations |
        Where-Object { $_ -match 'SoftTpmDriver\.sys' }
).Count -gt 0

if ($driverStagedForReboot)
{
    Write-Host 'NOTE: a rebuilt SoftTpmDriver.sys is staged but the loaded image is pinned until reboot.'
    Write-Host 'NOTE: everything below tests the PREVIOUS driver build; reboot and rerun to test the new one.'
}

Write-Step 'waiting for provisioning'
Start-Sleep -Seconds 8

Invoke-Step 'collect' {
    Copy-Item $ServiceLog "$LogDirectory\softtpm-service.log" -Force
    'collected softtpm.log'
}

Invoke-Step 'diagnose' {
    '== service status =='
    Get-Service SoftTpmService -ErrorAction SilentlyContinue |
        Format-List Name,Status,StartType | Out-String

    '== service log tail =='
    Get-Content $ServiceLog -Tail 35 -ErrorAction SilentlyContinue | Out-String

    '== SoftTpmStatus smoke =='
    $statusTool = Join-Path $SoftTpmRoot 'tools\SoftTpmStatus\bin\SoftTpmStatus.exe'
    if (Test-Path $statusTool) { & $statusTool 2>&1 | Out-String } else { "status tool not found at $statusTool" }

    '== state directory =='
    Get-ChildItem $StateDirectory -File -ErrorAction SilentlyContinue |
        Select-Object Name, Length, LastWriteTime |
        Format-Table -AutoSize | Out-String

    '== crash dumps =='
    Get-ChildItem $DumpDirectory -ErrorAction SilentlyContinue |
        Format-Table Name,Length,LastWriteTime | Out-String

    '== System log: Service Control Manager (last 15 min) =='
    Get-WinEvent -FilterHashtable @{LogName = 'System'; StartTime = (Get-Date).AddMinutes(-15) } -MaxEvents 500 -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -eq 'Service Control Manager' } |
        Select-Object -First 10 TimeCreated, Id, @{n = 'Message'; e = { ($_.Message -replace "`r`n", ' | ') } } |
        Format-List | Out-String

    '== Application log: errors (last 15 min) =='
    Get-WinEvent -FilterHashtable @{LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-15) } -MaxEvents 500 -ErrorAction SilentlyContinue |
        Where-Object { $_.LevelDisplayName -in @('Error', 'Critical') } |
        Select-Object -First 10 TimeCreated, ProviderName, Id, @{n = 'Message'; e = { ($_.Message -replace "`r`n", ' | ') } } |
        Format-List | Out-String

    '== WMI activity log (last 15 min) =='
    Get-WinEvent -FilterHashtable @{LogName = 'Microsoft-Windows-WMI-Activity/Operational'; StartTime = (Get-Date).AddMinutes(-15) } -MaxEvents 200 -ErrorAction SilentlyContinue |
        Where-Object { $_.LevelDisplayName -notin @('Information', 'Verbose') -or $_.Message -match 'Provider|Crash|ExitCode' } |
        Select-Object -First 12 TimeCreated, Id, LevelDisplayName, @{n = 'Message'; e = { ($_.Message -replace "`r`n", ' | ') } } |
        Format-List | Out-String

    '== TPM event logs =='
    try
    {
        $tpmLogs = Get-WinEvent -ListLog '*TPM*' -ErrorAction SilentlyContinue
        if ($tpmLogs)
        {
            $tpmLogs | Select-Object LogName, IsEnabled, RecordCount | Format-Table -AutoSize | Out-String
            foreach ($tpmLog in $tpmLogs)
            {
                if ($tpmLog.RecordCount -gt 0)
                {
                    "--- $($tpmLog.LogName) (last 10) ---"
                    Get-WinEvent -LogName $tpmLog.LogName -MaxEvents 10 -ErrorAction SilentlyContinue |
                        Select-Object TimeCreated, Id, LevelDisplayName, @{n = 'Message'; e = { ($_.Message -replace "`r`n", ' | ') } } |
                        Format-List | Out-String
                }
            }
        }
        else
        {
            'no TPM event logs found'
        }
    }
    catch
    {
        "TPM event log query failed: $($_.Exception.Message)"
    }

    '== WmiPrvSE processes =='
    Get-Process -Name WmiPrvSE -ErrorAction SilentlyContinue |
        Format-Table Id, StartTime, @{n = 'CPU'; e = { $_.CPU } } | Out-String
}

Invoke-Step 'get-tpm' {
    try { Get-Tpm | Format-List | Out-String } catch { "Get-Tpm failed: $($_.Exception.Message)" }
}

Invoke-Step 'ek-cert-info' {
    try
    {
        $ek = Get-TpmEndorsementKeyInfo -HashAlgorithm Sha256
        "IsPresent         : $($ek.IsPresent)"
        "IsDecryptionKey   : $($ek.IsDecryptionKey)"
        "IsSigningKey      : $($ek.IsSigningKey)"
        "ManufacturerCerts : $($ek.ManufacturerCertificates.Count)"
        "AdditionalCerts   : $($ek.AdditionalCertificates.Count)"
        foreach ($cert in $ek.ManufacturerCertificates)
        {
            ''
            "Subject    : $($cert.Subject)"
            "Issuer     : $($cert.Issuer)"
            "Thumbprint : $($cert.Thumbprint)"
            "NotBefore  : $($cert.NotBefore)"
            "NotAfter   : $($cert.NotAfter)"
        }
    }
    catch
    {
        "Get-TpmEndorsementKeyInfo failed: $($_.Exception.Message)"
    }

    '== Win32_Tpm =='
    try
    {
        $tpm = Get-CimInstance -Namespace 'root/cimv2/security/microsofttpm' -ClassName Win32_Tpm -ErrorAction Stop
        "SpecVersion      : $($tpm.SpecVersion)"
        "ManufacturerIdTxt: $($tpm.ManufacturerIdTxt)"
        "PhysicalPresence : $($tpm.PhysicalPresenceVersionInfo)"
        foreach ($method in 'IsEnabled', 'IsActivated', 'IsOwned')
        {
            $value = (Invoke-CimMethod -InputObject $tpm -MethodName $method -ErrorAction SilentlyContinue).$method
            "$method : $value"
        }
    }
    catch
    {
        "Win32_Tpm failed: $($_.Exception.Message)"
    }

    '== tpmtool =='
    foreach ($attempt in 1..2)
    {
        "attempt ${attempt}:"
        try { & tpmtool getdeviceinformation | Out-String } catch { "tpmtool failed: $($_.Exception.Message)" }
        "exit code: $LASTEXITCODE"
        if ($attempt -eq 1) { Start-Sleep -Seconds 5 }
    }
}

Invoke-Step 'wbcl' {
    try
    {
        $wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction Stop).'WBCL'
        "WBCL registry value present: $($wbcl.Length) bytes"
    }
    catch
    {
        'WBCL registry value not present'
    }

    $platformLog = Join-Path $env:ProgramData 'SoftTpm\platform-log.bin'
    if (Test-Path $platformLog)
    {
        "platform log file: $($platformLog) ($((Get-Item $platformLog).Length) bytes)"
    }
    else
    {
        'platform log file: not present (synthetic WBCL mode)'
    }

    $gatherDir = Join-Path $env:TEMP 'softtpm-tcglog'
    $capturedBootLog = Join-Path $env:TEMP 'softtpm-captured-bootlog.dat'
    $previousBootLog = Join-Path $gatherDir 'SRTMBoot.dat'
    if ((Test-Path $previousBootLog) -and ((Get-Item $previousBootLog).Length -gt 69))
    {
        Copy-Item $previousBootLog $capturedBootLog -Force
        "preserved the previously gathered boot log at $capturedBootLog ($((Get-Item $capturedBootLog).Length) bytes)"
    }

    Remove-Item $gatherDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $gatherDir -Force | Out-Null
    try { & tpmtool gatherlogs $gatherDir | Out-String } catch { "tpmtool gatherlogs failed: $($_.Exception.Message)" }
    "exit code: $LASTEXITCODE"
    Get-ChildItem $gatherDir -File -ErrorAction SilentlyContinue |
        ForEach-Object { "collected: $($_.Name) ($($_.Length) bytes)" }
}

Invoke-Step 'replay-test' {
    $platformLog = Join-Path $env:ProgramData 'SoftTpm\platform-log.bin'

    $sourceLog = @(
        (Join-Path $env:TEMP 'softtpm-tcglog\SRTMBoot.dat'),
        (Join-Path $env:TEMP 'softtpm-captured-bootlog.dat')
    ) |
        Where-Object { (Test-Path $_) -and ((Get-Item $_).Length -gt 69) } |
        Sort-Object { (Get-Item $_).Length } -Descending |
        Select-Object -First 1

    if (-not $sourceLog)
    {
        'no captured boot log larger than a synthetic WBCL is available; skipping the replay test'
        return
    }

    $sourceSize = (Get-Item $sourceLog).Length
    "replay source: $sourceLog ($sourceSize bytes)"
    Copy-Item $sourceLog $platformLog -Force
    "installed the captured log at $platformLog"

    $before = @(Get-Content $ServiceLog -ErrorAction SilentlyContinue).Count

    $service = Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped')
    {
        Stop-Service -Name 'SoftTpmService' -Force
        Start-Sleep -Seconds 2
    }

    Start-Service -Name 'SoftTpmService'

    $replayOk = $false
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline)
    {
        Start-Sleep -Seconds 1
        $newLines = @(Get-Content $ServiceLog -ErrorAction SilentlyContinue | Select-Object -Skip $before)

        if ($newLines -match 'platform: published')
        {
            $replayOk = [bool]($newLines -match 'platform log: replayed')
            break
        }

        $service = Get-Service -Name 'SoftTpmService' -ErrorAction SilentlyContinue
        if (-not $service -or $service.Status -ne 'Running')
        {
            break
        }
    }

    '== service log since the replay restart =='
    Get-Content $ServiceLog -ErrorAction SilentlyContinue | Select-Object -Skip $before

    if (-not $replayOk)
    {
        'REPLAY TEST FAILED: the captured log was not replayed (see the service log above)'
        return
    }

    try
    {
        $wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction Stop).'WBCL'
        if ($wbcl.Length -eq $sourceSize)
        {
            "REPLAY TEST PASSED: the published WBCL matches the captured log ($($wbcl.Length) bytes)"
        }
        else
        {
            "REPLAY TEST FAILED: the published WBCL is $($wbcl.Length) bytes, expected $sourceSize"
        }
    }
    catch
    {
        'REPLAY TEST FAILED: the WBCL registry value is missing after the replay restart'
    }

    Copy-Item $ServiceLog "$LogDirectory\softtpm-service-replay.log" -Force
    'archived the replay restart log as softtpm-service-replay.log'
}

if (-not $SkipCdb)
{
    Invoke-Step 'cdb' {
        $dump = Get-ChildItem (Join-Path $DumpDirectory '*.dmp') -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime | Select-Object -Last 1

        if (-not $dump)
        {
            'no crash dump found'
            return
        }

        if (-not (Test-Path $Debugger))
        {
            "debugger not found at $Debugger"
            return
        }

        & $Debugger -z $dump.FullName -y "$SoftTpmRoot\build\service\$Configuration" -c '.ecxr; kb; lm; q' 2>&1 |
            Select-Object -First 90
    }
}

if ($ConsoleDebug -and -not $SkipCdb)
{
    Invoke-Step 'console-debug' {
        if (-not (Test-Path $Debugger))
        {
            "debugger not found at $Debugger - skipping"
            return
        }

        $service = Get-Service SoftTpmService -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Stopped')
        {
            Stop-Service SoftTpmService -Force
            Start-Sleep -Seconds 2
        }

        $exe = Join-Path $SoftTpmRoot "build\service\$Configuration\SoftTpmService.exe"
        'launching console mode under cdb (up to 120s)'

        $commands = Join-Path $LogDirectory 'cdb-commands.txt'
        @(
            'bp kernel32!ExitProcess',
            'bp kernelbase!ExitProcess',
            'bp kernel32!TerminateProcess',
            'bp kernelbase!TerminateProcess',
            'bp kernel32!RaiseFailFastException',
            'bp kernelbase!RaiseFailFastException',
            'bp ntdll!RtlFailFast2',
            'bp ntdll!NtTerminateProcess',
            'sxe c0000005',
            'sxe c0000409',
            'g',
            'kb',
            'kp',
            '.exr -1',
            '.kill',
            'q'
        ) | Set-Content -Path $commands -Encoding ASCII

        $capture = Join-Path $LogDirectory 'cdb-console.log'
        $process = Start-Process -FilePath $Debugger `
            -ArgumentList @('-cf', $commands, '-G', $exe, '--console') `
            -PassThru -NoNewWindow `
            -RedirectStandardOutput $capture `
            -RedirectStandardError "$capture.err"

        if (-not $process.WaitForExit(120000))
        {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            'cdb timed out after 120s - killed (no crash seen?)'
        }
        else
        {
            "cdb exited with code $($process.ExitCode)"
        }

        '== cdb output (tail) =='
        Get-Content $capture -Tail 150 -ErrorAction SilentlyContinue

        '== cdb stderr (tail) =='
        Get-Content "$capture.err" -Tail 20 -ErrorAction SilentlyContinue

        '== service log tail (console run lines are appended) =='
        Get-Content $ServiceLog -Tail 30 -ErrorAction SilentlyContinue

        try
        {
            Start-Service SoftTpmService
            'service restarted after console-debug'
        }
        catch
        {
            "service restart failed: $($_.Exception.Message)"
        }
    }
}

if ($AttestationTest)
{
    Invoke-Step 'attestation-test' {
        '== trust the SoftTPM EK CA in LocalMachine\Root =='
        $caPath = Join-Path $StateDirectory 'ek-ca.pem'
        if (Test-Path $caPath)
        {
            & certutil.exe -addstore -f Root $caPath | Out-String
            "certutil addstore exit code: $LASTEXITCODE"
        }
        else
        {
            "CA certificate not found at $caPath"
        }

        '== tpmtool baseline (after CA trust) =='
        try { & tpmtool getdeviceinformation | Out-String } catch { "tpmtool failed: $($_.Exception.Message)" }
        "exit code: $LASTEXITCODE"

        '== TPM scheduled tasks =='
        foreach ($taskName in 'Tpm-Maintenance', 'AikCertEnrollTask')
        {
            try
            {
                $task = Get-ScheduledTask -TaskPath '\Microsoft\Windows\TPM\' -TaskName $taskName -ErrorAction Stop
                $taskInfo = $task | Get-ScheduledTaskInfo
                "$taskName : state=$($task.State) lastRun=$($taskInfo.LastRunTime) result=$('0x{0:X8}' -f [uint32]$taskInfo.LastTaskResult) nextRun=$($taskInfo.NextRunTime)"
            }
            catch
            {
                "$taskName : $($_.Exception.Message)"
            }
        }

        '== TPM WMI registry (TaskStates / endorsement) =='
        try
        {
            Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Services\TPM\WMI' -Recurse -ErrorAction Stop |
                ForEach-Object {
                    $_.Name
                    Get-ItemProperty -Path $_.PSPath -ErrorAction SilentlyContinue |
                        Format-List | Out-String
                }
        }
        catch
        {
            "registry query failed: $($_.Exception.Message)"
        }

        '== run Tpm-Maintenance task =='
        try
        {
            Start-ScheduledTask -TaskPath '\Microsoft\Windows\TPM\' -TaskName 'Tpm-Maintenance' -ErrorAction Stop
            Start-Sleep -Seconds 15
            $task = Get-ScheduledTask -TaskPath '\Microsoft\Windows\TPM\' -TaskName 'Tpm-Maintenance'
            $taskInfo = $task | Get-ScheduledTaskInfo
            "after run: state=$($task.State) result=$('0x{0:X8}' -f [uint32]$taskInfo.LastTaskResult) (async; reboot + rerun for settled state)"
        }
        catch
        {
            "maintenance run failed: $($_.Exception.Message)"
        }

        '== tpmtool after maintenance =='
        try { & tpmtool getdeviceinformation | Out-String } catch { "tpmtool failed: $($_.Exception.Message)" }
        "exit code: $LASTEXITCODE"

        '== Get-Tpm after maintenance =='
        try { Get-Tpm | Format-List | Out-String } catch { "Get-Tpm failed: $($_.Exception.Message)" }

        '== tpmdiagnostics (if installed) =='
        $tpmDiag = Get-Command 'tpmdiagnostics.exe' -ErrorAction SilentlyContinue
        if ($tpmDiag)
        {
            foreach ($diagCmd in 'CheckEkCertState', 'MaintenanceTaskStatus', 'IsWindowsAIKInstalledInNCrypt', 'EkInfo')
            {
                "--- tpmdiagnostics $diagCmd ---"
                try { & tpmdiagnostics.exe $diagCmd | Out-String } catch { "tpmdiagnostics $diagCmd failed: $($_.Exception.Message)" }
            }
        }
        else
        {
            'tpmdiagnostics not installed (optional: DISM /Online /Add-Capability /CapabilityName:Tpm.TpmDiagnostics~~~~0.0.1.0)'
        }

        '== TPM-backed key via Microsoft Platform Crypto Provider =='
        try
        {
            $cert = New-SelfSignedCertificate -Provider 'Microsoft Platform Crypto Provider' `
                -Subject 'CN=SoftTPM Attestation Probe' `
                -KeyAlgorithm RSA -KeyLength 2048 -KeyUsage DigitalSignature `
                -CertStoreLocation Cert:\LocalMachine\My -ErrorAction Stop
            "created TPM-backed cert: $($cert.Thumbprint) hasPrivateKey=$($cert.HasPrivateKey)"
            Remove-Item -Path "Cert:\LocalMachine\My\$($cert.Thumbprint)" -Force
            'probe certificate removed'
        }
        catch
        {
            "Platform Crypto Provider key failed: $($_.Exception.Message)"
        }

        '== tpmtool final =='
        try { & tpmtool getdeviceinformation | Out-String } catch { "tpmtool failed: $($_.Exception.Message)" }
        "exit code: $LASTEXITCODE"
    }
}

Invoke-Step 'collect-final' {
    Copy-Item $ServiceLog (Join-Path $LogDirectory 'softtpm-service-full.log') -Force
    'full service log copied after all probes'
}

Write-Step 'done'
"Step logs written to $LogDirectory (build-full.log, install-service.log, collect.log, diagnose.log, get-tpm.log, ek-cert-info.log, cdb.log, console-debug.log)"
"Service log copied to $LogDirectory\softtpm-service.log (first 8s) and softtpm-service-full.log (complete)"
"console-debug is opt-in: rerun with -ConsoleDebug for cdb deep-dive"
"attestation-test is opt-in: rerun with -AttestationTest to experiment with TPM attestation readiness"
