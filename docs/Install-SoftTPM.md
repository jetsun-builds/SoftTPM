# Installing SoftTPM

This guide installs SoftTPM on a Windows 10/11 x64 machine.
It is a test workflow, not a production deployment.

> **Do not use your primary PC.** SoftTPM replaces the platform TPM. Suspend
> or turn off BitLocker first, keep the recovery key, and expect Windows Hello
> and any TPM-sealed data to become unusable after a state reset.

## 1. Prepare the machine

1. Use a spare machine with Windows 10/11 x64 and administrator access.
2. Back up important data.
3. Suspend or disable BitLocker on every volume, and save the recovery keys.
4. Disable Secure Boot in UEFI setup. A test-signed kernel driver cannot load
   with Secure Boot enabled.
5. Make sure the firmware TPM is enabled and visible to Windows:
   ```powershell
   Get-Tpm
   tpmtool getdeviceinformation
   ```
   The firmware TPM must be working for the optional platform-log capture in
   step 4. If you do not need a real boot log, skip step 4.
6. Install Visual Studio 2022 with the "Desktop development with C++"
   workload, the Windows SDK and the WDK. Install CMake 3.16.3 or newer and a
   64-bit OpenSSL (1.1.1 or 3.x). The defaults are
   `C:\Program Files\OpenSSL-Win64` and CMake on `PATH`.
7. Open an elevated PowerShell window.

## 2. Get the sources

Place `softtpm` and Microsoft's `ms-tpm-20-ref` side by side. The build script
expects this exact layout:

```text
C:\src\SoftTpm\
  softtpm\
  ms-tpm-20-ref\
    TPMCmd\
```

```powershell
mkdir C:\src\SoftTpm
cd C:\src\SoftTpm
git clone https://github.com/microsoft/ms-tpm-20-ref.git
# copy or clone the softtpm directory next to ms-tpm-20-ref
cd C:\src\SoftTpm\softtpm
```

## 3. Build and sign

```powershell
cd C:\src\SoftTpm\softtpm
.\scripts\build.ps1 -OpenSslRoot "C:\Program Files\OpenSSL-Win64"
.\scripts\sign.ps1
```

`build.ps1` builds the TPM reference core, the service, the KMDF driver and
the `SoftTpmStatus` tool. `sign.ps1` creates or reuses the `SoftTpmTest`
code-signing certificate, trusts it locally, and signs the binaries.

Enable test signing and reboot:

```powershell
bcdedit /set testsigning on
Restart-Computer -Force
```

## 4. Capture the platform log (optional)

Do this **before** installing SoftTPM and **before** disabling the firmware
TPM. The capture reads the firmware's WBCL from the registry:

```powershell
cd C:\src\SoftTpm\softtpm
.\scripts\test-debug.ps1 -Mode Capture -OutputPath C:\temp\platform-log.bin
```

`C:\temp\platform-log.bin` is now a copy of the firmware boot log. SoftTPM
will validate and replay it after installation. Without it, SoftTPM publishes
a spec-ID-only log and PCRs stay at their reset values.

Do not run the capture after SoftTPM is installed: it would read SoftTPM's
own published log instead of the firmware's.

## 5. Install

After the reboot, open an elevated PowerShell again:

```powershell
cd C:\src\SoftTpm\softtpm
.\scripts\install.ps1 -DisableExistingTpm
```

`-DisableExistingTpm` disables the physical/firmware TPM node because the
driver and the firmware TPM both claim `\??\TPM`. If installation says a new
driver file is scheduled for the next boot, reboot and continue after the
reboot.

If you captured a log in step 4, install it into the state directory and
restart the service:

```powershell
Copy-Item C:\temp\platform-log.bin "$env:ProgramData\SoftTpm\platform-log.bin" -Force
Restart-Service SoftTpmService
```

The service validates the log, replays its measurements into the SHA-256,
SHA-384 and SHA-512 PCR banks, and publishes it as the WBCL.

## 6. Verify

```powershell
.\scripts\test-debug.ps1 -Mode Status
Get-Tpm
tpmtool getdeviceinformation
.\tools\SoftTpmStatus\bin\SoftTpmStatus.exe
```

Expected: `Get-Tpm` reports `TpmPresent`, `TpmReady`, `TpmEnabled` and
`TpmActivated` as `True`; `tpmtool getdeviceinformation` exits 0; the status
mode shows the WBCL (spec-ID-only or the captured log size).

Run the full harness for provisioning, replay and log collection:

```powershell
.\scripts\test-debug.ps1 -Mode Full
```

Logs are written under `logs/`; the service log is
`%ProgramData%\SoftTpm\softtpm.log`.

## 7. Reboot and validate the replay

Reboot, then run:

```powershell
cd C:\src\SoftTpm\softtpm
.\scripts\test-debug.ps1 -Mode Validate
```

This checks the post-boot service replay, requires the WBCL to contain
`platform-log.bin` when a captured log is installed (the validated run is
byte-identical), runs `tpmtool gatherlogs`, and reports attestation
readiness. It exits with code 1
if a required check fails. Results are in `logs/validate-reboot-replay.log`.

Inspect a log offline at any time:

```powershell
.\scripts\test-debug.ps1 -Mode Inspect -Path C:\temp\platform-log.bin
```

## 8. Uninstall and restore the firmware TPM

```powershell
.\scripts\uninstall.ps1 -EnableExistingTpm
```

This stops the service, removes the device and driver package, and re-enables
the firmware TPM. Add `-RemoveState` only if you also want to destroy the
software TPM identity, keys and EK certificate:

```powershell
.\scripts\uninstall.ps1 -RemoveState -EnableExistingTpm
```

After uninstalling, re-enable Secure Boot if you disabled it. If you enabled
test signing only for this test, turn it off again:

```powershell
bcdedit /set testsigning off
```

Reboot, re-enable BitLocker, and re-enroll Windows Hello if needed.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `install.ps1` refuses because a TPM exists | Disable the firmware TPM in Device Manager |
| Driver does not load | `bcdedit /enum {current}` must show `testsigning Yes`; Secure Boot must be off |
| Service does not start | `"%ProgramFiles%\SoftTpm\SoftTpmService.exe" --console` and `%ProgramData%\SoftTpm\softtpm.log` |
| No WBCL after install | Restart `SoftTpmService`; check the service log for the platform-log validation result |
| Replay validation fails | `.\scripts\test-debug.ps1 -Mode Inspect` and `logs/validate-reboot-replay.log` |
| BitLocker setup fails | The OS drive cannot use a software TPM at boot; use a password/USB protector, not a TPM protector |
