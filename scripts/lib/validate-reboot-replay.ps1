#Requires -RunAsAdministrator

<#
.SYNOPSIS
Validates the first boot after SoftTPM published a replayed WBCL.

.DESCRIPTION
Run this immediately after rebooting the SoftTPM system. SoftTpmService must
have started and replayed %ProgramData%\SoftTpm\platform-log.bin. The script:

  - finds the service start recorded for this boot and checks that it loaded,
    replayed and published the platform log without errors,
  - captures the current WBCL registry value and requires it to contain the
    platform log,
  - validates the captured log with the inspect mode (three-bank profile),
  - runs tpmtool gatherlogs and requires success (object-context exhaustion
    fix),
  - reports the newest MeasuredBoot log and attestation readiness.

Exits with code 1 if a required check fails.

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Validate

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Validate -SkipGatherLogs
#>

[CmdletBinding()]
param(
    [string]$PlatformLog = (Join-Path $env:ProgramData 'SoftTpm\platform-log.bin'),
    [string]$ServiceLog = (Join-Path $env:ProgramData 'SoftTpm\softtpm.log'),
    [switch]$SkipGatherLogs
)

$ErrorActionPreference = 'Stop'

$SoftTpmRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$LogDirectory = Join-Path $SoftTpmRoot 'logs'
New-Item -ItemType Directory -Force $LogDirectory | Out-Null
$ValidationLog = Join-Path $LogDirectory 'validate-reboot-replay.log'
$InspectScript = Join-Path $PSScriptRoot 'inspect-boot-log.ps1'

$script:checks = @()
$script:exitCode = 0

function Write-Step([string]$Message)
{
    Write-Output ''
    Write-Output "==> $Message"
}

function Read-Bytes([string]$FilePath)
{
    [System.IO.File]::ReadAllBytes($FilePath)
}

function Test-Prefix([byte[]]$Prefix, [byte[]]$Value)
{
    if ($Prefix.Length -gt $Value.Length)
    {
        return $false
    }

    for ($i = 0; $i -lt $Prefix.Length; $i++)
    {
        if ($Prefix[$i] -ne $Value[$i])
        {
            return $false
        }
    }

    return $true
}

function Add-Check([string]$Name, [bool]$Pass, [string]$Detail)
{
    $script:checks += [pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail }
    $label = 'FAIL'
    if ($Pass)
    {
        $label = 'PASS'
    }
    "  [$label] $Name - $Detail"
}

function Add-Info([string]$Message)
{
    "  [INFO] $Message"
}

function Invoke-Inspect([string]$FilePath, [string]$ArchiveName)
{
    $output = & $InspectScript -Path $FilePath | Out-String
    $inspectLog = Join-Path $LogDirectory 'inspect-boot-log.log'
    if (Test-Path $inspectLog)
    {
        Copy-Item $inspectLog (Join-Path $LogDirectory $ArchiveName) -Force
    }
    return $output
}

& {
    $bootTime = (Get-CimInstance -ClassName Win32_OperatingSystem).LastBootUpTime
    "boot time : $bootTime"
    "platform  : $PlatformLog"
    "service   : $ServiceLog"

    Write-Step 'platform log'
    $platformBytes = $null
    if (Test-Path $PlatformLog)
    {
        $platformBytes = Read-Bytes $PlatformLog
        "size: $($platformBytes.Length) bytes"
        Add-Check 'platform log present' $true "$($platformBytes.Length) bytes"
    }
    else
    {
        Add-Check 'platform log present' $false "missing $PlatformLog"
    }

    Write-Step 'service log since boot'
    $loadedBytes = -1
    $publishedBytes = -1
    if (Test-Path $ServiceLog)
    {
        $lines = @(Get-Content $ServiceLog)
        $startIndex = -1
        for ($i = $lines.Count - 1; $i -ge 0; $i--)
        {
            if ($lines[$i] -match 'service: starting')
            {
                $startIndex = $i
                break
            }
        }

        if ($startIndex -lt 0)
        {
            Add-Check 'service start found' $false 'no "service: starting" line'
        }
        else
        {
            $startLine = $lines[$startIndex]
            $bootLines = @($lines[$startIndex..($lines.Count - 1)])
            $bootLines | ForEach-Object { $_ }
            $bootText = $bootLines -join "`n"

            $startTime = $null
            if ($startLine -match '^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})')
            {
                $startTime = [datetime]::ParseExact(
                    $Matches[1],
                    'yyyy-MM-dd HH:mm:ss.fff',
                    [System.Globalization.CultureInfo]::InvariantCulture)
            }
            $afterBoot = ($null -ne $startTime) -and ($startTime -ge $bootTime.AddMinutes(-1))
            Add-Check 'service started after boot' $afterBoot "started $startTime"

            $loaded = [regex]::Match($bootText, 'platform log: loaded .*\((\d+) bytes, (\d+) digest algorithm\(s\)\)')
            $replayed = [regex]::Match($bootText, 'replayed (\d+) measurement event\(s\) out of (\d+)')
            $published = [regex]::Match($bootText, 'published the platform log as WBCL \((\d+) bytes\)')

            if ($loaded.Success)
            {
                $loadedBytes = [int]$loaded.Groups[1].Value
                $bankCount = [int]$loaded.Groups[2].Value
                $loadedOk = ($bankCount -eq 3)
                if ($platformBytes)
                {
                    $loadedOk = $loadedOk -and ($loadedBytes -eq $platformBytes.Length)
                }
                Add-Check 'service loaded the platform log' $loadedOk "$loadedBytes bytes, $bankCount digest algorithm(s)"
            }
            else
            {
                Add-Check 'service loaded the platform log' $false 'no load line after the last start'
            }

            if ($replayed.Success)
            {
                Add-Check 'service replayed measurement events' $true "$($replayed.Groups[1].Value) of $($replayed.Groups[2].Value) event(s)"
            }
            else
            {
                Add-Check 'service replayed measurement events' $false 'no replay line after the last start'
            }

            if ($published.Success)
            {
                $publishedBytes = [int]$published.Groups[1].Value
                $publishedOk = ($loadedBytes -eq $publishedBytes)
                Add-Check 'service published the platform log as WBCL' $publishedOk "$publishedBytes bytes"
            }
            else
            {
                Add-Check 'service published the platform log as WBCL' $false 'no publish line after the last start'
            }

            $errors = [regex]::Matches($bootText, 'unsupported digest algorithm|failed to replay|platform log: rejected|replay failed')
            Add-Check 'no replay errors after boot' ($errors.Count -eq 0) "$($errors.Count) error line(s)"

            Copy-Item $ServiceLog (Join-Path $LogDirectory 'softtpm-service-postreboot.log') -Force -ErrorAction SilentlyContinue
        }
    }
    else
    {
        Add-Check 'service log present' $false "missing $ServiceLog"
    }

    Write-Step 'WBCL registry value'
    $wbcl = $null
    try
    {
        $wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction Stop).'WBCL'
    }
    catch
    {
        $wbcl = $null
    }

    if (-not $wbcl)
    {
        Add-Check 'WBCL present' $false 'registry value missing'
    }
    else
    {
        $capturePath = Join-Path $env:TEMP 'softtpm-postreboot-wbcl.dat'
        [System.IO.File]::WriteAllBytes($capturePath, $wbcl)
        Add-Check 'WBCL captured' $true "$($wbcl.Length) bytes -> $capturePath"

        if ($platformBytes)
        {
            if (($wbcl.Length -eq $platformBytes.Length) -and (Test-Prefix $platformBytes $wbcl))
            {
                Add-Check 'WBCL matches the platform log' $true 'byte-for-byte'
            }
            elseif (($wbcl.Length -gt $platformBytes.Length) -and (Test-Prefix $platformBytes $wbcl))
            {
                Add-Check 'WBCL matches the platform log' $true "platform log is a prefix, $($wbcl.Length - $platformBytes.Length) appended byte(s)"
            }
            else
            {
                Add-Check 'WBCL matches the platform log' $false "$($wbcl.Length) bytes do not contain the platform log"
            }
        }

        $inspectOutput = Invoke-Inspect $capturePath 'inspect-wbcl-postreboot.log'
        $clean = $inspectOutput -match 'VERDICT: replays cleanly'
        Add-Check 'inspect-boot-log accepts the WBCL' $clean $(if ($clean) { 'verdict: replays cleanly' } else { 'see logs\inspect-wbcl-postreboot.log' })

        $algorithmCount = ([regex]::Matches($inspectOutput, 'algorithm\[\d+\]:')).Count
        $hasSha512 = $inspectOutput -match 'SHA512'
        Add-Check 'WBCL declares three banks incl. SHA-512' (($algorithmCount -ge 3) -and $hasSha512) "algorithms: $algorithmCount, SHA-512: $hasSha512"
    }

    if (-not $SkipGatherLogs)
    {
        Write-Step 'tpmtool gatherlogs'
        $gatherDir = Join-Path $env:TEMP 'softtpm-postreboot-tcglog'
        Remove-Item $gatherDir -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path $gatherDir -Force | Out-Null
        $gatherOutput = $null
        $gatherExit = 1
        try
        {
            $gatherOutput = & tpmtool gatherlogs $gatherDir | Out-String
            $gatherExit = $LASTEXITCODE
        }
        catch
        {
            $gatherOutput = "tpmtool gatherlogs failed: $($_.Exception.Message)"
        }

        if ($gatherOutput)
        {
            $gatherOutput
        }
        Add-Check 'tpmtool gatherlogs succeeds' ($gatherExit -eq 0) "exit code $gatherExit"

        Get-ChildItem $gatherDir -File -ErrorAction SilentlyContinue |
            ForEach-Object { "  collected: $($_.Name) ($($_.Length) bytes)" }

        $srtm = Join-Path $gatherDir 'SRTMBoot.dat'
        if ((Test-Path $srtm) -and $wbcl)
        {
            $srtmBytes = Read-Bytes $srtm
            Add-Check 'gathered SRTMBoot.dat matches the WBCL' (($srtmBytes.Length -eq $wbcl.Length) -and (Test-Prefix $wbcl $srtmBytes)) "$($srtmBytes.Length) bytes"
        }
    }

    Write-Step 'attestation readiness'
    $deviceInfo = ''
    try
    {
        $deviceInfo = & tpmtool getdeviceinformation | Out-String
    }
    catch
    {
        $deviceInfo = ''
    }
    $deviceInfo
    $ready = $deviceInfo -match 'Ready For Attestation:\s+True'
    $capable = $deviceInfo -match 'Is Capable For Attestation:\s+True'
    Add-Check 'Ready For Attestation' ($ready -and $capable) "ready=$ready capable=$capable"

    Write-Step 'MeasuredBoot log for this boot'
    $measuredRoot = Join-Path $env:SystemRoot 'Logs\MeasuredBoot'
    $measured = @(
        Get-ChildItem $measuredRoot -Filter *.log -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending
    )

    if ($measured.Count -eq 0)
    {
        Add-Info "no .log files under $measuredRoot"
    }
    else
    {
        $newest = $measured[0]
        Add-Info "newest: $($newest.Name) ($($newest.Length) bytes, $($newest.LastWriteTime))"
        if ($newest.LastWriteTime -ge $bootTime.AddMinutes(-5))
        {
            $measuredOutput = Invoke-Inspect $newest.FullName 'inspect-measuredboot-postreboot.log'
            $measuredClean = $measuredOutput -match 'VERDICT: replays cleanly'
            Add-Info "MeasuredBoot verdict: $(if ($measuredClean) { 'replays cleanly' } else { 'see logs\inspect-measuredboot-postreboot.log' })"
            $measuredAlgorithms = ([regex]::Matches($measuredOutput, 'algorithm\[\d+\]:')).Count
            $measuredSha512 = $measuredOutput -match 'SHA512'
            Add-Info "MeasuredBoot algorithms: $measuredAlgorithms, SHA-512: $measuredSha512"
        }
        else
        {
            Add-Info 'newest MeasuredBoot log predates the current boot'
        }
    }

    ''
    $failed = @($script:checks | Where-Object { -not $_.Pass })
    if ($failed.Count -eq 0)
    {
        "RESULT: PASS ($($script:checks.Count) checks)"
        $script:exitCode = 0
    }
    else
    {
        "RESULT: FAIL ($($failed.Count) of $($script:checks.Count) checks failed)"
        $script:exitCode = 1
    }
} 2>&1 | Tee-Object -FilePath $ValidationLog

exit $script:exitCode
