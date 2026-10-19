<#
.SYNOPSIS
Inspects a captured TCG2 event log so you can see what SoftTPM would replay.

.DESCRIPTION
SoftTPM parses platform log files with little-endian field rules, validates
them, and replays every measurement event into its PCRs with TPM2_PCR_Extend.
This script runs the same checks offline and reports the header, the digest
algorithms, every event, the per-PCR histogram and any condition that would
make SoftTPM reject the log or stop mid-replay.

By default it inspects the logs captured by the test-debug harness
(%TEMP%\softtpm-captured-bootlog.dat, %TEMP%\softtpm-tcglog\SRTMBoot.dat)
and the active %ProgramData%\SoftTpm\platform-log.bin.

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Inspect

.EXAMPLE
.\scripts\test-debug.ps1 -Mode Inspect -Path C:\temp\platform-log.bin
#>

[CmdletBinding()]
param(
    [string[]]$Path
)

$ErrorActionPreference = 'Stop'

$SoftTpmRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$LogDirectory = Join-Path $SoftTpmRoot 'logs'
New-Item -ItemType Directory -Force $LogDirectory | Out-Null
$InspectLog = Join-Path $LogDirectory 'inspect-boot-log.log'

$AlgorithmNames = @{
    0x0004 = 'SHA1'
    0x000B = 'SHA256'
    0x000C = 'SHA384'
    0x000D = 'SHA512'
    0x0012 = 'SM3_256'
}

$AlgorithmSizes = @{
    0x000B = 32
    0x000C = 48
    0x000D = 64
}

$EventTypeNames = @{
    0x00000000 = 'EV_PREBOOT_CERT'
    0x00000001 = 'EV_POST_CODE'
    0x00000003 = 'EV_NO_ACTION'
    0x00000004 = 'EV_SEPARATOR'
    0x0000000D = 'EV_EFI_VARIABLE_DRIVER_CONFIG'
    0x0000000E = 'EV_EFI_VARIABLE_BOOT'
    0x80000001 = 'EV_EFI_BOOT_SERVICES_APPLICATION'
    0x80000002 = 'EV_EFI_BOOT_SERVICES_DRIVER'
    0x80000003 = 'EV_EFI_RUNTIME_SERVICES_DRIVER'
    0x80000004 = 'EV_EFI_GPT_EVENT'
    0x80000005 = 'EV_EFI_ACTION'
    0x80000006 = 'EV_EFI_PLATFORM_FIRMWARE_BLOB'
    0x80000007 = 'EV_EFI_HANDOFF_TABLES'
    0x80000008 = 'EV_EFI_HCRTM_EVENT'
    0x800000E0 = 'EV_EFI_VARIABLE_AUTHORITY'
}

function Format-ByteRange([byte[]]$Bytes, [int]$Offset, [int]$Count)
{
    $end = [Math]::Min($Offset + $Count, $Bytes.Length)

    if ($end -le $Offset)
    {
        return ''
    }

    ($Offset..($end - 1) | ForEach-Object { $Bytes[$_].ToString('X2') }) -join ' '
}

function Get-AlgorithmName([uint16]$Algorithm)
{
    if ($AlgorithmNames.ContainsKey([int]$Algorithm))
    {
        return ('{0} (0x{1:X4})' -f $AlgorithmNames[[int]$Algorithm], $Algorithm)
    }

    return ('unknown (0x{0:X4})' -f $Algorithm)
}

function Get-EventTypeName([uint32]$EventType)
{
    if ($EventTypeNames.ContainsKey([long]$EventType))
    {
        return $EventTypeNames[[long]$EventType]
    }

    return ('0x{0:X8}' -f $EventType)
}

function Inspect-TcgLog([string]$FilePath)
{
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    $size = $bytes.Length

    "file     : $FilePath"
    "size     : $size bytes"
    "md5      : $((Get-FileHash -Path $FilePath -Algorithm MD5).Hash)"
    "first 64 : $(Format-ByteRange $bytes 0 64)"
    ''

    if ($size -lt 65 -or $size -gt 2MB)
    {
        'REJECTED: size outside the supported range (65 .. 2097152 bytes)'
        return
    }

    $pcrIndex = [BitConverter]::ToUInt32($bytes, 0)
    $eventType = [BitConverter]::ToUInt32($bytes, 4)

    if ($pcrIndex -ne 0 -or $eventType -ne 3)
    {
        ('REJECTED: the first record is PCR {0}, event type {1}; expected PCR 0, EV_NO_ACTION (0x00000003)' -f $pcrIndex, (Get-EventTypeName $eventType))
        return
    }

    for ($i = 8; $i -lt 28; $i++)
    {
        if ($bytes[$i] -ne 0)
        {
            'REJECTED: the first record digest is not 20 zero bytes'
            return
        }
    }

    $eventSize = [BitConverter]::ToUInt32($bytes, 28)

    if ($eventSize -lt 29 -or $eventSize -gt ($size - 32))
    {
        "REJECTED: invalid spec ID event size ($eventSize)"
        return
    }

    $signature = ([System.Text.Encoding]::ASCII.GetString($bytes, 32, 16)).TrimEnd([char]0)

    if ($signature -ne 'Spec ID Event03')
    {
        "REJECTED: unsupported header signature (`"$signature`"); expected `"Spec ID Event03`""
        return
    }

    $platformClass = [BitConverter]::ToUInt32($bytes, 48)
    $versionMinor = $bytes[52]
    $versionMajor = $bytes[53]
    $errata = $bytes[54]
    $uintnSize = $bytes[55]
    $algorithmCount = [BitConverter]::ToUInt32($bytes, 56)

    if ($algorithmCount -lt 1 -or $algorithmCount -gt 16)
    {
        "REJECTED: invalid algorithm count ($algorithmCount)"
        return
    }

    if (28 + 4 * $algorithmCount + 1 -gt $eventSize)
    {
        'REJECTED: the algorithm table extends past the spec ID event'
        return
    }

    $vendorInfoSize = $bytes[32 + 28 + 4 * $algorithmCount]

    if (28 + 4 * $algorithmCount + 1 + $vendorInfoSize -ne $eventSize)
    {
        'REJECTED: the spec ID event size does not match its contents'
        return
    }

    "signature: $signature (major $versionMajor minor $versionMinor errata $errata uintn $uintnSize, platform class $platformClass)"
    "vendor info size: $vendorInfoSize, spec ID event size: $eventSize"
    ''

    $logAlgorithms = @{}
    $unsupported = @{}

    for ($i = 0; $i -lt $algorithmCount; $i++)
    {
        $algId = [BitConverter]::ToUInt16($bytes, 32 + 28 + 4 * $i)
        $digestSize = [BitConverter]::ToUInt16($bytes, 32 + 30 + 4 * $i)
        "algorithm[$i]: $(Get-AlgorithmName $algId), digest size $digestSize"

        if ($algId -eq 0x0004)
        {
            '  note: SHA1 is not in the engine''s PCR banks; a log declaring SHA1 is rejected'
        }

        if ($AlgorithmSizes.ContainsKey([int]$algId))
        {
            $logAlgorithms[[int]$algId] = [int]$digestSize
        }
        else
        {
            $unsupported[[int]$algId] = $true
        }
    }

    ''
    'walking events:'

    $offset = 32 + $eventSize
    $eventTotal = 0
    $extendTotal = 0
    $pcrExtends = @{}
    $typeCounts = @{}
    $details = New-Object System.Collections.Generic.List[string]
    $failure = $null

    while ($offset -lt $size)
    {
        if ($offset + 12 -gt $size)
        {
            $failure = "truncated event header at offset $offset"
            break
        }

        $evPcr = [BitConverter]::ToUInt32($bytes, $offset)
        $evType = [BitConverter]::ToUInt32($bytes, $offset + 4)
        $evCount = [BitConverter]::ToUInt32($bytes, $offset + 8)

        if ($evCount -gt 8)
        {
            $failure = "event $eventTotal at offset $offset lists $evCount digests (maximum 8)"
            break
        }

        if ($evPcr -gt 23)
        {
            $failure = "event $eventTotal extends invalid PCR $evPcr"
            break
        }

        if ($evPcr -ge 17 -and $evPcr -le 22)
        {
            $failure = "event $eventTotal extends DRTM PCR $evPcr (not reachable from locality 0)"
            break
        }

        $cursor = $offset + 12
        $algorithms = @()
        $ok = $true

        for ($d = 0; $d -lt $evCount; $d++)
        {
            if ($cursor + 2 -gt $size)
            {
                $failure = "truncated digest list in event $eventTotal"
                $ok = $false
                break
            }

            $algId = [BitConverter]::ToUInt16($bytes, $cursor)
            $cursor += 2

            if (-not $logAlgorithms.ContainsKey([int]$algId))
            {
                $failure = "event $eventTotal uses algorithm 0x$($algId.ToString('X4')) which is not in the spec ID table"
                $ok = $false
                break
            }

            $digestSize = $logAlgorithms[[int]$algId]

            if ($cursor + $digestSize -gt $size)
            {
                $failure = "truncated digest in event $eventTotal"
                $ok = $false
                break
            }

            $cursor += $digestSize
            $algorithms += $algId
        }

        if (-not $ok)
        {
            break
        }

        if ($cursor + 4 -gt $size)
        {
            $failure = "truncated event data size in event $eventTotal"
            break
        }

        $dataSize = [BitConverter]::ToUInt32($bytes, $cursor)
        $cursor += 4

        if ($cursor + $dataSize -gt $size)
        {
            $failure = "truncated event data in event $eventTotal"
            break
        }

        $cursor += $dataSize
        $eventTotal++

        if ($typeCounts.ContainsKey([uint32]$evType))
        {
            $typeCounts[[uint32]$evType]++
        }
        else
        {
            $typeCounts[[uint32]$evType] = 1
        }

        if ($evType -ne 3 -and $evCount -gt 0)
        {
            $extendTotal++

            if ($pcrExtends.ContainsKey([uint32]$evPcr))
            {
                $pcrExtends[[uint32]$evPcr]++
            }
            else
            {
                $pcrExtends[[uint32]$evPcr] = 1
            }
        }

        if ($details.Count -lt 15)
        {
            $digestNames = ($algorithms | ForEach-Object { (Get-AlgorithmName $_).Split(' ')[0] }) -join '+'
            $details.Add(('event {0,4}: PCR {1,2} {2} digests: {3}' -f $eventTotal, $evPcr, (Get-EventTypeName $evType), $digestNames))
        }

        $offset = $cursor
    }

    $details
    if ($eventTotal -gt 15)
    {
        "... ($($eventTotal - 15) more events)"
    }

    ''
    "events parsed   : $eventTotal"
    "events extended : $extendTotal"

    foreach ($pcr in ($pcrExtends.Keys | Sort-Object))
    {
        ('PCR {0,2}          : {1} extend(s)' -f $pcr, $pcrExtends[$pcr])
    }

    'event types    :'
    foreach ($type in ($typeCounts.Keys | Sort-Object))
    {
        ('  {0} x{1}' -f (Get-EventTypeName $type), $typeCounts[$type])
    }

    ''
    if ($failure)
    {
        "VERDICT: SoftTPM stops replaying here - $failure"
    }
    elseif ($unsupported.Count -gt 0)
    {
        'VERDICT: SoftTPM rejects this log - the spec ID table lists an algorithm the engine cannot extend'
    }
    else
    {
        "VERDICT: replays cleanly - $extendTotal measurement event(s) into $($pcrExtends.Keys.Count) PCR(s)"
    }
}

& {
    if ($Path)
    {
        $files = $Path
    }
    else
    {
        $files = @(
            (Join-Path $env:TEMP 'softtpm-captured-bootlog.dat'),
            (Join-Path $env:TEMP 'softtpm-tcglog\SRTMBoot.dat'),
            (Join-Path $env:ProgramData 'SoftTpm\platform-log.bin')
        )
    }

    $inspected = 0

    foreach ($file in $files)
    {
        if (-not (Test-Path $file))
        {
            "skipping missing $file"
            continue
        }

        if ($inspected -gt 0)
        {
            ''
            ('=' * 72)
            ''
        }

        Inspect-TcgLog $file
        $inspected++
    }

    ''
    ('=' * 72)
    ''
    'MeasuredBoot directory:'
    Get-ChildItem 'C:\Windows\Logs\MeasuredBoot' -ErrorAction SilentlyContinue |
        Select-Object Name, Length, LastWriteTime |
        Format-Table -AutoSize | Out-String

    'Current WBCL registry value:'
    $wbcl = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\IntegrityServices' -Name 'WBCL' -ErrorAction SilentlyContinue).'WBCL'

    if ($wbcl)
    {
        "size     : $($wbcl.Length) bytes"
        "first 32 : $(Format-ByteRange $wbcl 0 32)"
    }
    else
    {
        'not present'
    }
} 2>&1 | Tee-Object -FilePath $InspectLog
