[CmdletBinding()]
param(
    [string]$OpenSslRoot = $env:SOFTTPM_OPENSSL_ROOT,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipDriver,
    [switch]$SkipTool,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$env:MSBUILDDISABLENODEREUSE = '1'

function Write-Step([string]$Message)
{
    Write-Output ''
    Write-Output "==> $Message"
}

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
        & $FilePath @remaining 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() }
            else { $_ }
        }
        $script:ExternalExitCode = $LASTEXITCODE
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
}

function Find-OpenSslRoot
{
    param([string]$Requested)

    $candidates = @()
    if ($Requested) { $candidates += $Requested }
    $candidates += @(
        'C:\OpenSSL-Win64',
        'C:\OpenSSL',
        'C:\Program Files\OpenSSL-Win64',
        'C:\Program Files\OpenSSL'
    )

    foreach ($candidate in $candidates)
    {
        if ($candidate -and (Test-Path (Join-Path $candidate 'include\openssl')))
        {
            return (Resolve-Path $candidate).Path
        }
    }

    throw 'OpenSSL was not found. Pass -OpenSslRoot <path> pointing at an OpenSSL x64 installation (1.1.1 or 3.x, with include\openssl).'
}

function Find-OpenSslLibDir
{
    param([string]$Root)

    foreach ($libDir in @(
        (Join-Path $Root 'lib\x64'),
        (Join-Path $Root 'lib\VC\x64\MD'),
        (Join-Path $Root 'lib\VC\x64'),
        (Join-Path $Root 'lib')))
    {
        if (Test-Path (Join-Path $libDir 'libcrypto.lib'))
        {
            return $libDir
        }
    }

    throw "libcrypto.lib was not found under $Root."
}

function Find-OpenSslDllDir
{
    param([string]$Root, [string]$LibDir)

    foreach ($dir in @($LibDir, (Join-Path $Root 'bin'), $Root))
    {
        if (Get-ChildItem $dir -Filter '*crypto*.dll' -ErrorAction SilentlyContinue)
        {
            return $dir
        }
    }

    return $LibDir
}

function Find-CMake
{
    $command = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere)
    {
        $installs = & $vswhere -latest -property installationPath 2>$null
        foreach ($install in @($installs))
        {
            $candidate = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    foreach ($candidate in @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files (x86)\CMake\bin\cmake.exe'))
    {
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

function Find-MSBuild
{
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere)
    {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
            Select-Object -First 1
        if ($found) { return $found }

        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
        if ($found) { return $found }
    }

    $command = Get-Command 'msbuild.exe' -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    return $null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$softTpmRoot = Split-Path -Parent $scriptDir
$repoRoot = Split-Path -Parent $softTpmRoot

$buildDir = Join-Path $softTpmRoot 'build'
$tpmSrc = Join-Path $buildDir 'tpm-src'
$tpmSrcRoot = Join-Path $tpmSrc 'TPMCmd'
$tpmConfig = Join-Path $buildDir 'tpm-config'
$tpmBuild = Join-Path $buildDir 'tpm-build'
$tpmPrefix = Join-Path $buildDir 'tpm-prefix'
$serviceBuild = Join-Path $buildDir 'service'

if ($Clean -and (Test-Path $buildDir))
{
    Write-Step "Removing $buildDir"
    Remove-Item $buildDir -Recurse -Force
}

$vendoredTpm = Join-Path $repoRoot 'ms-tpm-20-ref\TPMCmd'
if (-not (Test-Path $vendoredTpm))
{
    throw "Vendored ms-tpm-20-ref was not found at $vendoredTpm"
}

Write-Step 'Locating OpenSSL'
$openssl = Find-OpenSslRoot -Requested $OpenSslRoot
$opensslLibDir = Find-OpenSslLibDir -Root $openssl
$opensslDllDir = Find-OpenSslDllDir -Root $openssl -LibDir $opensslLibDir
Write-Host "OpenSSL: $openssl"
Write-Host "  lib: $opensslLibDir"
Write-Host "  dll: $opensslDllDir"

Write-Step 'Locating CMake'
$cmakeExe = Find-CMake
if (-not $cmakeExe)
{
    throw 'CMake was not found. Install CMake, or add the CMake component to Visual Studio (VS bundles it under Common7\IDE\CommonExtensions\Microsoft\CMake).'
}
Write-Host "CMake: $cmakeExe"

Write-Step 'Staging a private copy of the reference TPM source'
if (-not (Test-Path $tpmSrcRoot))
{
    New-Item -ItemType Directory -Path $tpmSrc -Force | Out-Null
    Copy-Item $vendoredTpm $tpmSrcRoot -Recurse
}

$osslInclude = Join-Path $tpmSrcRoot 'OsslInclude\x64'
New-Item -ItemType Directory -Path $osslInclude -Force | Out-Null
$osslIncludeTarget = Join-Path $osslInclude 'openssl'
if (Test-Path $osslIncludeTarget)
{
    Remove-Item $osslIncludeTarget -Recurse -Force
}
Copy-Item (Join-Path $openssl 'include\openssl') $osslIncludeTarget -Recurse -Force

$osslLib = Join-Path $tpmSrcRoot 'lib\x64'
New-Item -ItemType Directory -Path $osslLib -Force | Out-Null
Copy-Item (Join-Path $opensslLibDir 'libcrypto.lib') $osslLib -Force
Get-ChildItem $opensslDllDir -Filter '*crypto*.dll' -ErrorAction SilentlyContinue |
    Copy-Item -Destination $osslLib -Force

Write-Step 'Preparing the SoftTPM TpmConfiguration'
Invoke-External $cmakeExe `
    "-DSoftTpm_TpmSourceDir=$tpmSrcRoot" `
    "-DSoftTpm_ConfigOutDir=$tpmConfig" `
    -P (Join-Path $softTpmRoot 'service\cmake\PrepareTpmConfiguration.cmake')
if ($script:ExternalExitCode -ne 0) { throw 'PrepareTpmConfiguration.cmake failed' }

Write-Step "Building ms-tpm-20-ref ($Configuration, lib only)"
Invoke-External $cmakeExe -S $tpmSrcRoot -B $tpmBuild -A x64 `
    "-Duser_TpmConfiguration_Dir=$tpmConfig" `
    '-DTpm_BuildOption_LibOnly=1' `
    "-DCMAKE_INSTALL_PREFIX=$tpmPrefix"
if ($script:ExternalExitCode -ne 0) { throw 'cmake configure of ms-tpm-20-ref failed' }

Invoke-External $cmakeExe --build $tpmBuild --config $Configuration
if ($script:ExternalExitCode -ne 0) { throw 'build of ms-tpm-20-ref failed' }

Invoke-External $cmakeExe --install $tpmBuild --config $Configuration
if ($script:ExternalExitCode -ne 0) { throw 'install of ms-tpm-20-ref failed' }

Write-Step "Building SoftTpmService ($Configuration)"
Invoke-External $cmakeExe -S (Join-Path $softTpmRoot 'service') -B $serviceBuild -A x64 `
    "-DSoftTpm_TpmPrefix=$tpmPrefix" `
    "-DSoftTpm_OpenSslRoot=$openssl" `
    "-DSoftTpm_OpenSslLibDir=$opensslLibDir" `
    "-DSoftTpm_OpenSslDllDir=$opensslDllDir"
if ($script:ExternalExitCode -ne 0) { throw 'cmake configure of SoftTpmService failed' }

# Same shared-folder timestamp skew as the driver: touch the service sources so
# the build cannot skip recompiling edited files.
Get-ChildItem (Join-Path $softTpmRoot 'service') -Recurse -File -Include '*.c', '*.h', '*.txt' -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LastWriteTime = Get-Date }

Invoke-External $cmakeExe --build $serviceBuild --config $Configuration
if ($script:ExternalExitCode -ne 0) { throw 'build of SoftTpmService failed' }

if (-not $SkipDriver)
{
    Write-Step "Building SoftTpmDriver ($Configuration)"

    $msbuildExe = Find-MSBuild
    if (-not $msbuildExe)
    {
        throw 'MSBuild was not found. Install Visual Studio 2022 with the WDK or pass -SkipDriver.'
    }

    $now = Get-Date
    $driverVer = '1.0.{0}.{1}' -f $now.ToString('MMdd'), $now.ToString('HHmm')
    $vcxprojPath = Join-Path $softTpmRoot 'driver\SoftTpmDriver.vcxproj'
    $vcxprojText = Get-Content $vcxprojPath -Raw
    $vcxprojText = $vcxprojText -replace '<TimeStamp>[^<]*</TimeStamp>', ('<TimeStamp>' + $driverVer + '</TimeStamp>')
    Set-Content -Path $vcxprojPath -Value $vcxprojText -Encoding UTF8
    Write-Host "Driver version for this build: $driverVer"

    # File timestamps on the shared folder can look older than the build outputs,
    # which makes MSBuild skip compilation. Touch the sources to force a rebuild.
    Get-ChildItem (Join-Path $softTpmRoot 'driver') -Recurse -File -Include '*.c', '*.h', '*.inf', '*.rc' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.LastWriteTime = $now }

    Invoke-External $msbuildExe (Join-Path $softTpmRoot 'driver\SoftTpmDriver.vcxproj') `
        "/p:Configuration=$Configuration" `
        '/p:Platform=x64' `
        '/m'
    if ($script:ExternalExitCode -ne 0) { throw 'build of SoftTpmDriver failed' }
    else
    {
        Write-Host 'The driver must be test-signed before it can load: .\scripts\test-sign.ps1'
    }
}

if (-not $SkipTool)
{
    Write-Step 'Building SoftTpmStatus'

    $toolProject = Join-Path $softTpmRoot 'tools\SoftTpmStatus\SoftTpmStatus.csproj'

    $msbuildExe = Find-MSBuild
    if ($msbuildExe)
    {
        Invoke-External $msbuildExe $toolProject "/p:Configuration=$Configuration"
        if ($script:ExternalExitCode -ne 0) { throw 'build of SoftTpmStatus failed' }
    }
    else
    {
        $dotnet = Get-Command 'dotnet.exe' -ErrorAction SilentlyContinue
        if (-not $dotnet)
        {
            Write-Warning 'Neither MSBuild nor dotnet was found; skipping the SoftTpmStatus tool.'
        }
        else
        {
            Invoke-External $dotnet.Source build $toolProject -c $Configuration
            if ($script:ExternalExitCode -ne 0) { throw 'build of SoftTpmStatus failed' }
        }
    }
}

Write-Step 'Done'
Write-Host "Service:  $serviceBuild\$Configuration\SoftTpmService.exe"
if (-not $SkipDriver)
{
    Write-Host "Driver:   $softTpmRoot\driver\x64\$Configuration\SoftTpmDriver.sys"
}
Write-Host "Tool:     $softTpmRoot\tools\SoftTpmStatus\bin\SoftTpmStatus.exe"
Write-Host ''
Write-Host 'Next: .\scripts\test-sign.ps1'
Write-Host '      .\scripts\install.ps1'
