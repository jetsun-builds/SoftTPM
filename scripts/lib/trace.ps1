#Requires -RunAsAdministrator
#
# SoftTPM diagnostics: service state, device state, driver trace ring, and a
# direct driver test that bypasses TBS (opens \\.\TPM and issues the two
# TBS-facing IOCTLs by hand).
#
# Usage (elevated PowerShell):  .\scripts\test-debug.ps1 -Mode Trace

$ErrorActionPreference = 'Stop'

$ioctlTpmSubmit     = 0x22C00C
$ioctlTpmDeviceInfo = 0x22C01C
$ioctlGetStatus     = 0x22C06408
$ioctlReadTrace     = 0x22C0640C

$nativeType = 'SoftTpmDiag.Native' -as [type]
if ($nativeType -and -not $nativeType.GetMethod('DeviceIoControlEx')) {
    throw 'An older SoftTpmDiag.Native is loaded in this PowerShell session (it lacks the new helpers). Open a fresh admin window and rerun trace.ps1.'
}
if (-not $nativeType) {
Add-Type -Namespace SoftTpmDiag -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool DeviceIoControl(IntPtr hDevice, uint dwIoControlCode, byte[] lpInBuffer, uint nInBufferSize, byte[] lpOutBuffer, uint nOutBufferSize, out uint lpBytesReturned, IntPtr lpOverlapped);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(IntPtr hObject);

// Capture the real Win32 error inside the managed call, before PowerShell
// can clobber the thread's last-error value.
public static int CreateFileEx(string lpFileName, uint dwDesiredAccess, out IntPtr handle)
{
    handle = CreateFileW(lpFileName, dwDesiredAccess, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (handle == new IntPtr(-1))
    {
        return Marshal.GetLastWin32Error();
    }
    return 0;
}

public static int DeviceIoControlEx(IntPtr hDevice, uint dwIoControlCode, byte[] lpInBuffer, uint nInBufferSize, byte[] lpOutBuffer, uint nOutBufferSize, out uint lpBytesReturned)
{
    if (DeviceIoControl(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer, nOutBufferSize, out lpBytesReturned, IntPtr.Zero))
    {
        return 0;
    }
    return Marshal.GetLastWin32Error();
}
'@
}

$GENERIC_READ  = [uint32]2147483648
$GENERIC_WRITE = [uint32]1073741824
function Show-Win32Error([string]$What, [int]$Code) {
    Write-Output ("  {0} failed, Win32 error {1}" -f $What, $Code)
}

function Read-SoftTpmStatus {
    $handle = [IntPtr]::Zero
    $err = [SoftTpmDiag.Native]::CreateFileEx('\\.\SoftTpmControl', $GENERIC_READ -bor $GENERIC_WRITE, [ref]$handle)
    if ($err -ne 0) { return $null }
    try {
        $buffer = New-Object byte[] 28
        $returned = [uint32]0
        $rc = [SoftTpmDiag.Native]::DeviceIoControlEx($handle, $ioctlGetStatus, $null, 0, $buffer, 28, [ref]$returned)
        if ($rc -ne 0) { return $null }
        return @{
            Protocol     = [BitConverter]::ToUInt32($buffer, 0)
            State        = [BitConverter]::ToUInt32($buffer, 4)
            Pending      = [BitConverter]::ToUInt32($buffer, 8)
            Completed    = [BitConverter]::ToUInt32($buffer, 12)
            Timeouts     = [BitConverter]::ToUInt32($buffer, 16)
            UnknownIoctl = [BitConverter]::ToUInt32($buffer, 20)
            LastRc       = [BitConverter]::ToUInt32($buffer, 24)
        }
    } finally {
        [SoftTpmDiag.Native]::CloseHandle($handle) | Out-Null
    }
}

function Format-SoftTpmStatus($Status) {
    if ($null -eq $Status) { return 'unavailable' }
    $stateName = 'UNKNOWN'
    if ($Status.State -eq 1) { $stateName = 'STOPPED (no service)' }
    if ($Status.State -eq 2) { $stateName = 'RUNNING (service connected)' }
    return ('protocol={0} state={1} [{2}] pending={3} completed={4} timeouts={5} unknownIoctl={6} lastRc=0x{7:X8}' -f `
        $Status.Protocol, $Status.State, $stateName, $Status.Pending, $Status.Completed, `
        $Status.Timeouts, $Status.UnknownIoctl, $Status.LastRc)
}

Write-Output '== service =='
& sc.exe query SoftTpmService

Write-Output ''
Write-Output '== installed driver file =='
$installedPath = 'C:\Windows\System32\drivers\SoftTpmDriver.sys'
$sys = Get-Item $installedPath -ErrorAction SilentlyContinue
$installedHash = $null
if ($sys) {
    $installedHash = (Get-FileHash $installedPath -Algorithm MD5).Hash
    Write-Output ("  {0}  {1} bytes  {2}" -f $sys.LastWriteTime, $sys.Length, $sys.FullName)
    Write-Output ("  md5 {0}" -f $installedHash)
    $installedSig = Get-AuthenticodeSignature $installedPath
    Write-Output ("  signature: {0} ({1})" -f $installedSig.Status, $installedSig.StatusMessage)
} else {
    Write-Output '  not present in System32\drivers'
}

$packageSys = Join-Path $PSScriptRoot '..\..\driver\x64\Release\SoftTpmDriver\SoftTpmDriver.sys'
if (Test-Path $packageSys) {
    $packageHash = (Get-FileHash $packageSys -Algorithm MD5).Hash
    Write-Output ("  package sys: {0} bytes  md5 {1}" -f (Get-Item $packageSys).Length, $packageHash)
    $packageSig = Get-AuthenticodeSignature $packageSys
    Write-Output ("  package signature: {0} ({1})" -f $packageSig.Status, $packageSig.StatusMessage)
    if ($installedHash) {
        Write-Output ("  System32 matches package: {0}" -f ($installedHash -eq $packageHash))
    }
} else {
    Write-Output '  package sys not visible (run trace from the share so Z: is available)'
}

Write-Output ''
Write-Output '== pnp device =='
$dev = Get-PnpDevice -InstanceId 'ROOT\SECURITYDEVICES\0000' -ErrorAction SilentlyContinue
if ($dev) {
    Write-Output ("  Status={0} Problem={1} ({2})" -f $dev.Status, $dev.Problem, $dev.ProblemDescription)
    foreach ($key in 'DEVPKEY_Device_ProblemStatus', 'DEVPKEY_Device_DevNodeStatus') {
        try {
            $property = Get-PnpDeviceProperty -InstanceId 'ROOT\SECURITYDEVICES\0000' -KeyName $key -ErrorAction Stop
            if ($null -ne $property.Data) {
                Write-Output ("  {0} = 0x{1:X8}" -f $key, [uint32]$property.Data)
            }
        } catch {
            Write-Output ("  {0} unavailable" -f $key)
        }
    }
} else {
    Write-Output '  device not found'
}

Write-Output ''
Write-Output '== TPM stack binary evidence (tbs.dll / tbs.sys / tpm.sys) =='
function Find-BinaryText([string]$Path, [string]$Text) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $hits = @()
    if ([Text.Encoding]::Unicode.GetString($bytes).Contains($Text)) { $hits += 'utf16' }
    if ([Text.Encoding]::ASCII.GetString($bytes).Contains($Text)) { $hits += 'ascii' }
    return $hits
}
foreach ($candidate in @(
    (Join-Path $env:SystemRoot 'System32\drivers\tbs.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\tpm.sys'),
    (Join-Path $env:SystemRoot 'System32\tbs.dll')
)) {
    $leaf = Split-Path $candidate -Leaf
    if (-not (Test-Path $candidate)) {
        Write-Output ("  {0}: not present" -f $leaf)
        continue
    }
    $foundAny = $false
    foreach ($needle in '\??\TPM', '\Device\TPM') {
        $hits = Find-BinaryText $candidate $needle
        if ($hits.Count -gt 0) {
            $foundAny = $true
            Write-Output ("  {0}: '{1}' ({2})" -f $leaf, $needle, ($hits -join ', '))
        }
    }
    if (-not $foundAny) {
        Write-Output ("  {0}: no TPM device-name strings found" -f $leaf)
    }
}

Write-Output ''
Write-Output '== TPM IOCTL immediates in client/provider binaries =='
function Count-U32LittleEndian([byte[]]$Bytes, [uint32]$Value) {
    $encoding = [Text.Encoding]::GetEncoding(28591)
    $needle = $encoding.GetString([BitConverter]::GetBytes($Value))
    $haystack = $encoding.GetString($Bytes)
    $count = 0
    $index = $haystack.IndexOf($needle)
    while ($index -ge 0) {
        $count++
        $index = $haystack.IndexOf($needle, $index + 1)
    }
    return $count
}
$ioctlProbes = [ordered]@{
    'cancel(1)'      = [uint32]0x22C004
    'submit(3)'      = [uint32]0x22C00C
    'ppi(5)'         = [uint32]0x22C014
    'devinfo(7)'     = [uint32]0x22C01C
    'submitNB(65)'   = [uint32]0x22C194
    'morbit(66)'     = [uint32]0x22C198
    'intinfo(68)'    = [uint32]0x22C1A0
}
foreach ($candidate in @(
    (Join-Path $env:SystemRoot 'System32\drivers\tbs.sys'),
    (Join-Path $env:SystemRoot 'System32\drivers\tpm.sys'),
    (Join-Path $env:SystemRoot 'System32\tbs.dll'),
    (Join-Path $env:SystemRoot 'System32\TpmCoreProvisioning.dll')
)) {
    $leaf = Split-Path $candidate -Leaf
    if (-not (Test-Path $candidate)) {
        Write-Output ("  {0}: not present" -f $leaf)
        continue
    }
    $bytes = [IO.File]::ReadAllBytes($candidate)
    $results = foreach ($probe in $ioctlProbes.GetEnumerator()) {
        ('{0}={1}' -f $probe.Key, (Count-U32LittleEndian $bytes $probe.Value))
    }
    Write-Output ("  {0}: {1}" -f $leaf, ($results -join '  '))
}

Write-Output ''
Write-Output '== driver init diagnostics (registry) =='
try {
    $driverKey = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\SoftTpmDriver' -ErrorAction Stop
    $found = $false
    $driverKey.PSObject.Properties | Where-Object { $_.Name -like 'Dbg*' } | Sort-Object Name | ForEach-Object {
        $found = $true
        Write-Output ('  {0} = 0x{1:X8}' -f $_.Name, [uint32]$_.Value)
    }
    if (-not $found) {
        Write-Output '  no Dbg* values: the driver never wrote them (see error-log breadcrumbs below)'
    }
} catch {
    Write-Output '  driver service key not readable'
}

Write-Output ''
Write-Output '== Kernel-PnP events (SoftTpm) =='
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-PnP' } -MaxEvents 80 -ErrorAction Stop |
        Where-Object { $_.Message -match 'SoftTpm' } |
        Select-Object -First 6 |
        ForEach-Object { Write-Output ('  {0} [{1}] {2}' -f $_.TimeCreated, $_.Id, (($_.Message -replace "`r`n", ' ') -replace '  +', ' ')) }
} catch {
    Write-Output '  Kernel-PnP events unavailable'
}

Write-Output ''
Write-Output '== WDF runtime state =='
foreach ($wdfFile in 'Wdf01000.sys', 'WdfLdr.sys') {
    $wdfPath = Join-Path $env:SystemRoot ('System32\drivers\' + $wdfFile)
    if (Test-Path $wdfPath) {
        $vi = (Get-Item $wdfPath).VersionInfo
        Write-Output ('  {0}: FileVersion={1}' -f $wdfFile, $vi.FileVersion)
    } else {
        Write-Output ('  {0}: not present' -f $wdfFile)
    }
}
foreach ($svcName in 'Wdf01000', 'SoftTpmDriver') {
    $svc = Get-CimInstance Win32_SystemDriver -Filter ("Name='{0}'" -f $svcName) -ErrorAction SilentlyContinue
    if ($svc) {
        Write-Output ('  {0} service: State={1} Status={2} StartMode={3}' -f $svcName, $svc.State, $svc.Status, $svc.StartMode)
    } else {
        Write-Output ('  {0} service: not found' -f $svcName)
    }
}
$scOutput = & sc.exe query SoftTpmDriver 2>&1
foreach ($line in $scOutput) { Write-Output ('  sc: ' + $line) }
try {
    Get-WinEvent -LogName 'System' -MaxEvents 200 -ErrorAction Stop |
        Where-Object { $_.ProviderName -eq 'Service Control Manager' -and $_.Message -match 'SoftTpm' } |
        Select-Object -First 5 |
        ForEach-Object { Write-Output ('  SCM [{0}] {1}' -f $_.Id, (($_.Message -replace "`r`n", ' ') -replace '  +', ' ')) }
} catch {
    Write-Output '  SCM event scan unavailable'
}

Write-Output ''
Write-Output '== SoftTpmDriver error-log breadcrumbs =='
$breadcrumbs = @()
try {
    $breadcrumbs = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'SoftTpmDriver' } -MaxEvents 20 -ErrorAction Stop
} catch {
    try {
        $breadcrumbs = Get-WinEvent -LogName 'System' -MaxEvents 500 -ErrorAction Stop |
            Where-Object { $_.ProviderName -eq 'SoftTpmDriver' } |
            Select-Object -First 20
    } catch {
        $breadcrumbs = @()
    }
}
if ($breadcrumbs) {
    foreach ($event in $breadcrumbs) {
        $values = ($event.Properties | ForEach-Object { $_.Value }) -join ' '
        Write-Output ("  {0} id=0x{1:X4} {2}" -f $event.TimeCreated, $event.Id, $values)
    }
} else {
    Write-Output '  none: the driver entry point never reached a breadcrumb (or this build predates them)'
}

Write-Output ''
Write-Output '== CodeIntegrity events (SoftTpm) =='
try {
    Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 40 -ErrorAction Stop |
        Where-Object { $_.Message -match 'SoftTpm' } |
        Select-Object -First 6 |
        ForEach-Object { Write-Output ('  {0} [{1}] {2}' -f $_.TimeCreated, $_.Id, (($_.Message -replace "`r`n", ' ') -replace '  +', ' ')) }
} catch {
    Write-Output '  CodeIntegrity operational log unavailable'
}

Write-Output ''
Write-Output '== setupapi.dev.log (last SoftTpm lines) =='
try {
    Select-String -Path (Join-Path $env:SystemRoot 'inf\setupapi.dev.log') -Pattern 'SoftTpm' |
        Select-Object -Last 12 |
        ForEach-Object { Write-Output ('  ' + $_.Line.Trim()) }
} catch {
    Write-Output '  setupapi.dev.log not readable'
}

Write-Output ''
Write-Output '== service log (last 15 lines) =='
$logPath = Join-Path $env:ProgramData 'SoftTpm\softtpm.log'
if (Test-Path $logPath) {
    try {
        Get-Content $logPath -Tail 15 | ForEach-Object { Write-Output ("  " + $_) }
    } catch {
        Write-Output ('  cannot read {0} (locked by the running service)' -f $logPath)
    }
} else {
    Write-Output ('  {0} not found' -f $logPath)
}

Write-Output ''
Write-Output '== control device: status + trace =='
$statusBefore = Read-SoftTpmStatus
if ($statusBefore) {
    Write-Output ('  ' + (Format-SoftTpmStatus $statusBefore))
} else {
    Write-Output '  control status read failed'
}

$control = [IntPtr]::Zero
$err = [SoftTpmDiag.Native]::CreateFileEx('\\.\SoftTpmControl', $GENERIC_READ -bor $GENERIC_WRITE, [ref]$control)
if ($err -ne 0) {
    Show-Win32Error 'CreateFile(\\.\SoftTpmControl)' $err
} else {
    $traceBuf = New-Object byte[] (128 * 32)
    $returned = [uint32]0
    $rc = [SoftTpmDiag.Native]::DeviceIoControlEx($control, $ioctlReadTrace, $null, 0, $traceBuf, $traceBuf.Length, [ref]$returned)
    if ($rc -eq 0) {
        $count = [int]($returned / 32)
        Write-Output ("  trace entries: {0}" -f $count)
        for ($i = 0; $i -lt $count; $i++) {
            $o = $i * 32
            $inSize = [BitConverter]::ToUInt32($traceBuf, $o + 8)
            $dataHex = ''
            if ($inSize -gt 0) {
                $n = [Math]::Min(12, [int]$inSize)
                $dataHex = ' data=' + (($traceBuf[($o + 20)..($o + 19 + $n)] | ForEach-Object { $_.ToString('X2') }) -join ' ')
            }
            Write-Output ("    seq={0,-5} ioctl=0x{1:X8} in={2,-6} out={3,-6} status=0x{4:X8}{5}" -f `
                [BitConverter]::ToUInt32($traceBuf, $o),     [BitConverter]::ToUInt32($traceBuf, $o + 4), `
                $inSize, [BitConverter]::ToUInt32($traceBuf, $o + 12), `
                [BitConverter]::ToUInt32($traceBuf, $o + 16), $dataHex)
        }
    } else {
        Show-Win32Error 'DeviceIoControl(READ_TRACE)' $rc
    }
    [SoftTpmDiag.Native]::CloseHandle($control) | Out-Null
}

Write-Output ''
Write-Output '== direct TPM device test (bypasses TBS) =='
$tpm = [IntPtr]::Zero
$err = [SoftTpmDiag.Native]::CreateFileEx('\\.\TPM', $GENERIC_READ -bor $GENERIC_WRITE, [ref]$tpm)
if ($err -ne 0) {
    Show-Win32Error 'CreateFile(\\.\TPM)' $err
    $tpm = [IntPtr]::Zero
    $err = [SoftTpmDiag.Native]::CreateFileEx('\\?\GLOBALROOT\Device\SoftTpmTpm', $GENERIC_READ -bor $GENERIC_WRITE, [ref]$tpm)
    if ($err -ne 0) {
        Show-Win32Error 'CreateFile(\\?\GLOBALROOT\Device\SoftTpmTpm)' $err
    } else {
        Write-Output '  opened the device via GLOBALROOT instead of \\.\TPM'
    }
}

if ($err -eq 0) {
    Write-Output '  opened the TPM device'

    $infoBuf = New-Object byte[] 16
    $returned = [uint32]0
    $rc = [SoftTpmDiag.Native]::DeviceIoControlEx($tpm, $ioctlTpmDeviceInfo, $null, 0, $infoBuf, 16, [ref]$returned)
    if ($rc -eq 0) {
        Write-Output ("  GET_DEVICE_INFO ok, {0} bytes: structVersion={1} tpmVersion={2} tpmInterfaceType={3} tpmImpRevision={4}" -f `
            $returned, [BitConverter]::ToUInt32($infoBuf, 0), [BitConverter]::ToUInt32($infoBuf, 4), `
            [BitConverter]::ToUInt32($infoBuf, 8), [BitConverter]::ToUInt32($infoBuf, 12))
    } else {
        Show-Win32Error 'DeviceIoControl(GET_DEVICE_INFO)' $rc
    }

    # TPM2_GetCapability(cap=TPM_PROPERTIES, property=TPM_PT_FAMILY_INDICATOR, count=1)
    $cmd = [byte[]]@(
        0x80, 0x01, 0x00, 0x00, 0x00, 0x16,
        0x00, 0x00, 0x01, 0x7A,
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x01)
    $respBuf = New-Object byte[] 4096
    $returned = [uint32]0
    $rc = [SoftTpmDiag.Native]::DeviceIoControlEx($tpm, $ioctlTpmSubmit, $cmd, $cmd.Length, $respBuf, $respBuf.Length, [ref]$returned)
    if ($rc -eq 0) {
        $hex = ($respBuf[0..([Math]::Max(0, [int]$returned - 1))] | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Output ("  SUBMIT ok, {0} bytes: {1}" -f $returned, $hex)
        if ($returned -ge 10) {
            $respSize = ([uint32]$respBuf[2] -shl 24) -bor ([uint32]$respBuf[3] -shl 16) -bor ([uint32]$respBuf[4] -shl 8) -bor [uint32]$respBuf[5]
            $respCode = ([uint32]$respBuf[6] -shl 24) -bor ([uint32]$respBuf[7] -shl 16) -bor ([uint32]$respBuf[8] -shl 8) -bor [uint32]$respBuf[9]
            Write-Output ("  TPM response: size={0} code=0x{1:X8}" -f $respSize, $respCode)
        }
    } else {
        Show-Win32Error 'DeviceIoControl(SUBMIT_COMMAND)' $rc
        if ($returned -gt 0) {
            $hex = ($respBuf[0..([Math]::Max(0, [int]$returned - 1))] | ForEach-Object { $_.ToString('X2') }) -join ' '
            Write-Output ("  partial response: {0} bytes: {1}" -f $returned, $hex)
        }
    }

    [SoftTpmDiag.Native]::CloseHandle($tpm) | Out-Null
}

Write-Output ''
$statusAfter = Read-SoftTpmStatus
if ($statusAfter) {
    Write-Output ('  status after direct test: ' + (Format-SoftTpmStatus $statusAfter))
    if ($statusBefore) {
        Write-Output ('  delta: completed +{0} unknownIoctl +{1}' -f `
            ($statusAfter.Completed - $statusBefore.Completed), `
            ($statusAfter.UnknownIoctl - $statusBefore.UnknownIoctl))
    }
} else {
    Write-Output '  status after direct test: unavailable'
}

Write-Output ''
Write-Output 'Trace tool finished.'
