[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-Path([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing required path: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}
function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$dist = Join-Path $root 'dist'
if (-not (Test-Path -LiteralPath $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }
Start-Transcript -Path (Join-Path $dist 'build-package.log')
try {
    $vswhere = Require-Path "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ toolchain not found' }
    $msbuild = Require-Path (Join-Path $vs 'MSBuild/Current/Bin/amd64/MSBuild.exe')
    $kits = Require-Path "${env:ProgramFiles(x86)}/Windows Kits/10"
    $sdk = '10.0.26100.0'
    $null = Require-Path (Join-Path $kits "Include/$sdk/km/wdm.h")
    $signtool = Require-Path (Join-Path $kits "bin/$sdk/x64/signtool.exe")
    $inf2cat = Require-Path (Join-Path $kits 'bin/x86/Inf2Cat.exe')

    $opensslCommand = Get-Command openssl.exe -ErrorAction SilentlyContinue
    $candidates = @('C:/Program Files/OpenSSL', 'C:/Program Files/OpenSSL-Win64', 'C:/OpenSSL-Win64', 'C:/OpenSSL')
    if ($opensslCommand) { $candidates = @((Split-Path (Split-Path $opensslCommand.Source -Parent) -Parent)) + $candidates }
    $openssl = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'include/openssl/opensslv.h') } | Select-Object -First 1
    if (-not $openssl) { throw 'OpenSSL headers and libraries were not found on runner' }
    $openssl = Require-Path $openssl
    Write-Host "OpenSSL: $openssl; SDK/WDK: $sdk"
    $env:CMAKE_GENERATOR = 'Visual Studio 17 2022'

    # Keep the upstream local-development defaults; override the SDK only for CI.
    & (Require-Path (Join-Path $root 'scripts/build.ps1')) -OpenSslRoot $openssl -Configuration Release -SkipDriver
    Invoke-Checked $msbuild @((Join-Path $root 'driver/SoftTpmDriver.vcxproj'), '/m', '/p:Configuration=Release', '/p:Platform=x64', "/p:WindowsTargetPlatformVersion=$sdk", '/p:SignMode=Off', '/p:Inf2CatUseLocalTime=true')

    $package = Join-Path $dist 'SoftTPM-Windows11-x64-TestSigned'
    foreach ($folder in @($package, (Join-Path $package 'driver'), (Join-Path $package 'service'), (Join-Path $package 'tools'), (Join-Path $package 'licenses'))) {
        if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    }
    $driverBuild = Require-Path (Join-Path $root 'driver/x64/Release')
    $sys = Require-Path (Join-Path $driverBuild 'SoftTpmDriver.sys')
    Copy-Item -LiteralPath $sys -Destination (Join-Path $package 'driver/SoftTpmDriver.sys')
    $infCandidates = @((Join-Path $driverBuild 'SoftTpmDriver/SoftTpmDriver.inf'), (Join-Path $driverBuild 'SoftTpmDriver.inf'))
    $inf = $infCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $inf) { throw 'Stamped driver INF not found' }
    Copy-Item -LiteralPath $inf -Destination (Join-Path $package 'driver/SoftTpmDriver.inf')
    $serviceBuild = Require-Path (Join-Path $root 'build/service/Release')
    foreach ($file in Get-ChildItem -LiteralPath $serviceBuild -File | Where-Object { $_.Extension -in '.exe','.dll' }) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $package 'service')
    }
    Copy-Item -LiteralPath (Require-Path (Join-Path $root 'tools/SoftTpmStatus/bin/SoftTpmStatus.exe')) -Destination (Join-Path $package 'tools')

    # App-local Microsoft CRT avoids requiring a compiler/VC redist install on the PC.
    $redistRoot = Require-Path (Join-Path $vs 'VC/Redist/MSVC')
    $crt = Get-ChildItem -LiteralPath $redistRoot -Directory | Sort-Object Name -Descending | ForEach-Object {
        $candidate = Join-Path $_.FullName 'x64/Microsoft.VC143.CRT'
        if (Test-Path -LiteralPath $candidate) { $candidate }
    } | Select-Object -First 1
    if (-not $crt) { throw 'Redistributable x64 CRT directory not found' }
    foreach ($file in Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $package 'service') }
    $cryptoDll = @(Get-ChildItem -LiteralPath (Join-Path $package 'service') -Filter '*crypto*.dll' -File)
    if ($cryptoDll.Count -eq 0) { throw 'OpenSSL runtime DLL missing from package' }

    Copy-Item -LiteralPath (Require-Path (Join-Path $root 'LICENSE')) -Destination (Join-Path $package 'licenses/SoftTPM-LICENSE.txt')
    $engineRoot = Require-Path (Join-Path (Split-Path $root -Parent) 'ms-tpm-20-ref')
    foreach ($file in Get-ChildItem -LiteralPath $engineRoot -File | Where-Object { $_.Name -match 'LICENSE|NOTICE' }) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $package ('licenses/TPM-engine-' + $file.Name)) }
    $opensslLicense = Get-ChildItem -LiteralPath $openssl -Recurse -File | Where-Object { $_.Name -match '^(LICENSE|LICENSE.txt|LICENSE.md)$' } | Select-Object -First 1
    if (-not $opensslLicense) { throw 'OpenSSL license missing from runner installation' }
    Copy-Item -LiteralPath $opensslLicense.FullName -Destination (Join-Path $package 'licenses/OpenSSL-LICENSE.txt')
    foreach ($name in @('Install-SoftTPM.ps1','Uninstall-SoftTPM.ps1','Test-SoftTPM.ps1','Package-Common.ps1','NativeDevice.cs','README-zh-CN.md')) {
        Copy-Item -LiteralPath (Require-Path (Join-Path $PSScriptRoot $name)) -Destination $package
    }

    # The certificate private key is deliberately non-exportable and never uploaded.
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=SoftTPM jetsun-builds test $env:GITHUB_RUN_ID" -CertStoreLocation 'Cert:/CurrentUser/My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
    $certFile = Join-Path $package 'SoftTPM-TestSigning.cer'
    Export-Certificate -Cert $cert -FilePath $certFile | Out-Null
    foreach ($store in @('Cert:/LocalMachine/Root','Cert:/LocalMachine/TrustedPublisher')) {
        if (-not (Test-Path -LiteralPath $store)) { throw "Missing certificate store $store" }
        Import-Certificate -FilePath $certFile -CertStoreLocation $store | Out-Null
    }
    $signFiles = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object { $_.Name -in 'SoftTpmDriver.sys','SoftTpmService.exe','SoftTpmStatus.exe' })
    foreach ($file in $signFiles) { Invoke-Checked $signtool @('sign','/fd','SHA256','/sha1',$cert.Thumbprint,'/s','My',$file.FullName) }
    # Generate the catalog AFTER embedding the signature in the final driver binary.
    Invoke-Checked $inf2cat @("/driver:$(Join-Path $package 'driver')", '/os:10_X64', '/uselocaltime')
    $cat = Require-Path (Join-Path $package 'driver/SoftTpmDriver.cat')
    Invoke-Checked $signtool @('sign','/fd','SHA256','/sha1',$cert.Thumbprint,'/s','My',$cat)
    foreach ($file in $signFiles) { Invoke-Checked $signtool @('verify','/pa','/v',$file.FullName) }
    Invoke-Checked $signtool @('verify','/pa','/v','/c',$cat,(Join-Path $package 'driver/SoftTpmDriver.sys'))

    # Compile setup interop without invoking any device/BCD/TPM-changing method.
    Add-Type -Path (Require-Path (Join-Path $package 'NativeDevice.cs'))
    $helpProcess = Start-Process -FilePath (Require-Path (Join-Path $package 'service/SoftTpmService.exe')) -ArgumentList '--help' -WindowStyle Hidden -Wait -PassThru
    if ($helpProcess.ExitCode -ne 0) { throw "Service loader smoke test failed: $($helpProcess.ExitCode)" }
    $manifest = [ordered]@{
        repository = $env:GITHUB_REPOSITORY
        commit = $env:GITHUB_SHA
        run = "https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
        engineCommit = 'ee21db0a941decd3cac67925ea3310873af60ab3'
        sdk = $sdk
        target = 'Windows 11 x64 24H2/25H2/26H2; test signing required'
        certificateThumbprint = $cert.Thumbprint
        builtUtc = [DateTime]::UtcNow.ToString('o')
        validation = @('Release x64 build', 'Embedded signatures', 'Catalog driver hash and signature', 'Setup interop compilation', 'Service --help loader smoke test')
        notValidated = @('Kernel driver loading', 'Physical T7820 runtime', 'Windows Update 26H2 eligibility')
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding UTF8
    $hashes = Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $relative = $_.FullName.Substring($package.Length + 1).Replace('\','/')
        "$hash  $relative"
    }
    $hashes | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding ASCII
    @("## Windows 11 x64 test-signed package", "", "Commit: $env:GITHUB_SHA", "", "Download the SoftTPM-Windows11-x64-TestSigned artifact. Read README-zh-CN.md before installing.", "", "Certificate SHA-1 thumbprint: $($cert.Thumbprint)", "", "No driver was installed or loaded on the runner. Physical-machine runtime and Windows Update eligibility are not CI-validated.") | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
} finally { Stop-Transcript }
