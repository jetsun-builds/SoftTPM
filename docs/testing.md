# Testing and Debugging

All test and diagnostic workflows go through one script:

```powershell
.\scripts\test-debug.ps1 -Mode <mode>
```

It must run in an elevated PowerShell window. The internal helpers live in
`scripts/lib/`; do not call them directly from automation.

## Modes

| Mode | What it does | Side effects |
| --- | --- | --- |
| `Full` | End-to-end harness: build, install, provisioning checks, WBCL checks, replay test, log collection | Rebuilds and reinstalls unless switched off; restarts `SoftTpmService`; may install a captured log as `platform-log.bin` |
| `Capture` | Copies the current WBCL registry value into a file | None beyond writing the output file |
| `Inspect` | Parses a TCG2 log offline with the same rules the service uses | Writes `logs/inspect-boot-log.log` |
| `Validate` | Checks the first boot after a replayed log was published | Runs `tpmtool gatherlogs`; writes validation logs |
| `Trace` | Dumps service/device state, the driver trace ring and a direct TPM round trip | Opens `\\.\TPM` and the control device; no persistent changes |
| `Status` | Prints a quick snapshot of service, device, TPM stack, WBCL and platform log | None |
| `Session` | Dot-sources `$S`, `$L` and `Invoke-Step` into the current window | Sets the working directory to the repository root |

## Full harness

```powershell
.\scripts\test-debug.ps1 -Mode Full
```

The harness performs these steps:

1. Configures Windows Error Reporting local dumps and enables `WerSvc`.
2. Runs `build.ps1`. With `-ReinstallDriver` it builds the driver too;
   otherwise the driver build is skipped.
3. Runs `install.ps1`. With `-ReinstallDriver` the driver is reinstalled;
   otherwise only the service payload is updated.
4. Waits for provisioning, then collects `Get-Tpm`, EK certificate,
   `Win32_Tpm`, `tpmtool`, WBCL and event-log diagnostics.
5. Preserves a boot log gathered by `tpmtool gatherlogs`, installs it as
   `%ProgramData%\SoftTpm\platform-log.bin`, restarts the service and checks
   that the published WBCL matches the captured log size.
6. Optionally runs `cdb` against a crash dump (`-SkipCdb` disables this) and
   the console debugger path (`-ConsoleDebug`).
7. Optionally runs attestation probes (`-AttestationTest`): trusts the local
   EK CA, runs `Tpm-Maintenance`, and creates a TPM-backed certificate.

Useful switches:

| Switch | Effect |
| --- | --- |
| `-SkipBuild` | Use the existing binaries |
| `-ReinstallDriver` | Rebuild and reinstall the driver as well as the service |
| `-Configuration Debug` | Use Debug binaries |
| `-SkipCdb` | Do not inspect crash dumps |
| `-ConsoleDebug` | Run the service under `cdb` in console mode |
| `-AttestationTest` | Run the EK/attestation probes |
| `-OpenSslRoot <path>` | Override the OpenSSL root for the build |

The driver cannot be replaced while its previous image is loaded. When that
happens, `install.ps1` stages the new file for the next boot and the harness
warns that it is testing the previous build. Reboot and rerun.

## Status

```powershell
.\scripts\test-debug.ps1 -Mode Status
```

Prints:

- `SoftTpmService` state and `\\.\TPM` presence,
- the SoftTPM PnP device and its problem code,
- `Get-Tpm`,
- `SoftTpmStatus` (TBS device info plus EK/SRK public areas),
- `tpmtool getdeviceinformation`,
- the WBCL size and whether `platform-log.bin` is installed,
- the last 20 lines of the service log.

## Capture and inspect a platform log

```powershell
# While the firmware TPM is enabled and SoftTPM is not installed
.\scripts\test-debug.ps1 -Mode Capture -OutputPath C:\temp\platform-log.bin

# Offline validation; no service required
.\scripts\test-debug.ps1 -Mode Inspect -Path C:\temp\platform-log.bin
```

Without `-Path`, `Inspect` checks the logs captured by the harness
(`%TEMP%\softtpm-captured-bootlog.dat`, `%TEMP%\softtpm-tcglog\SRTMBoot.dat`)
and the active `%ProgramData%\SoftTpm\platform-log.bin`.

The inspector reports the header, digest algorithms, every event, the per-PCR
histogram and any condition that would make the service reject the log or
stop replaying it. It rejects:

- a log that is not a `Spec ID Event03` log for PCR 0,
- digest algorithms other than SHA-256/384/512,
- events for PCR 17-22 (DRTM, locality 0 only),
- inconsistent event sizes or a missing `EV_NO_ACTION` header.

## Validate after a reboot

```powershell
.\scripts\test-debug.ps1 -Mode Validate
```

Run this immediately after rebooting the SoftTPM machine. It verifies:

- the service boot record shows the log was loaded, replayed and published,
- the current WBCL contains `platform-log.bin` (byte-for-byte in the validated run),
- the log passes the same inspection (three-bank profile),
- `tpmtool gatherlogs` succeeds (no object-context exhaustion),
- the newest MeasuredBoot log and attestation readiness.

Options:

```powershell
.\scripts\test-debug.ps1 -Mode Validate -PlatformLog C:\temp\platform-log.bin
.\scripts\test-debug.ps1 -Mode Validate -SkipGatherLogs
```

The script exits with code 1 if a required check fails. The detailed result
is written to `logs/validate-reboot-replay.log`; the service log is archived
alongside it.

## Trace and interactive debugging

```powershell
.\scripts\test-debug.ps1 -Mode Trace
```

Trace mode reports service and device state, prints the driver trace ring
(unknown IOCTLs, timeouts, rejected responses, PPI calls) and submits a TPM
command directly to the driver, bypassing TBS. Use it when `Get-Tpm` or TBS
behaves differently from the raw device.

```powershell
. .\scripts\test-debug.ps1 -Mode Session
Invoke-Step "get-tpm" { Get-Tpm }
$L
```

Session mode is meant to be dot-sourced. `$S` is the repository root, `$L` is
the log directory, and `Invoke-Step` mirrors the harness logging helper.

## Logs

| Path | Contents |
| --- | --- |
| `%ProgramData%\SoftTpm\softtpm.log` | Service log (provisioning, replay, command errors) |
| `logs/` | Harness step logs, archived service logs, inspection and validation results |
| `%ProgramData%\SoftTpm\dumps` | WER dump folder configured by the harness |
| `logs/cdb-console.log` | Console debugger output when `-ConsoleDebug` is used |

## Expected results

On the validated Windows 11 x64 setup:

- `Get-Tpm` reports a present, ready, enabled, activated and owned TPM 2.0.
- `tpmtool getdeviceinformation` exits 0.
- `Tpm-Maintenance` completes with result 0 and `EkCertificatePresent = 1`.
- A captured three-bank log (92,139 bytes, 38 measurement events) replays
  without rejection.
- After a reboot the WBCL is byte-identical to `platform-log.bin`.
- `tpmtool gatherlogs` exits 0; the `MAX_LOADED_OBJECTS = 8` profile avoids
  `TPM_RC_OBJECT_MEMORY` (`0x80280902`).
- `-Mode Validate` passes all checks.
