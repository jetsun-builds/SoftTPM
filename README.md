# SoftTPM

<img width="3810" height="1790" alt="image" src="https://github.com/user-attachments/assets/c0ec8faf-5915-4c43-bcac-725e304a2a8c" />


SoftTPM is an open-source software TPM 2.0 for Windows. It presents a TPM 2.0
to the Windows TPM stack without TPM hardware: `tbs.sys` binds to it, and
every API built on TBS (`Win32_Tpm`, `tpm.msc`, `tpmtool`, the Microsoft
Platform Crypto Provider, BitLocker's runtime paths, Windows Hello) treats it
as an on-board TPM.

It consists of two components:

- `driver/` - a KMDF driver that owns `\??\TPM` (the device TBS opens) and
  implements the `tpm.sys`-compatible IOCTL surface: it forwards TPM commands
  to user mode, answers the device-information IOCTL, and provides a synthetic
  Physical Presence Interface responder.
- `service/` - a Windows service (LocalSystem) that hosts Microsoft's TPM 2.0
  reference implementation (`ms-tpm-20-ref`), persists TPM state across
  reboots, and provisions the Endorsement Key (EK), Storage Root Key (SRK)
  and EK certificate that Windows expects to find on a real TPM.

> **Note:** This is a hobby project. I use AI tools to help me push progress
> on it. Expect rough edges, and verify anything you depend on.
>
> Everyone is welcome to contribute: open issues, send pull requests, report
> bugs, suggest ideas, or improve the docs.

## How it works

```
 applications and OS features (Get-Tpm, tpm.msc, BitLocker, ...)
        |
   tbs.sys / tbs.dll                      Windows TPM Base Services
        |  \??\TPM
        v
 SoftTpmDriver (KMDF)          priority queue, timeout timer, trace ring
        |  \??\SoftTpmControl
        v
 SoftTpmService (LocalSystem)  ms-tpm-20-ref engine
                               state in %ProgramData%\SoftTpm
```

See [docs/architecture.md](docs/architecture.md) for the details.

## Status

SoftTPM is built and validated on Windows 11 x64 with the test-signed driver:

- `Get-Tpm` / `Win32_Tpm` report a ready, activated, owned TPM 2.0, and
  `tpmtool getdeviceinformation` works.
- TBS command traffic, `Tbsi_GetDeviceInfo`, and EK/SRK `ReadPublic` succeed;
  keys created through the Microsoft Platform Crypto Provider are TPM-backed.
- The scheduled `Tpm-Maintenance` task completes with status 0 and stores the
  self-issued EK certificate in the Windows `EKCertStore`.
- TPM state survives service restarts and machine reboots.

Known limits: `Ready For Attestation` stays false because there is no firmware
TCG event log, and BitLocker's OS-drive wizard refuses to start for the same
reason ("No TCG event is available"). A software TPM cannot unseal before the
OS boots, so OS-drive BitLocker with a TPM protector is out of scope. The full
list is in [docs/attestation-and-limits.md](docs/attestation-and-limits.md).

## Feature support

| Feature | Status |
| --- | --- |
| TPM discovery (`Get-Tpm`, `Win32_Tpm`, `tpm.msc`, `tpmtool`) | Works |
| TPM 2.0 command traffic through TBS (`GetCapability`, `ReadPublic`, key creation) | Works |
| EK/SRK provisioning and EK certificate at NV index `0x01C00002` | Works |
| Windows TPM maintenance (`Tpm-Maintenance`, `EKCertStore`) | Works (result 0, `EkCertificatePresent = 1`) |
| TPM-backed keys through the Microsoft Platform Crypto Provider | Works at runtime (software root of trust) |
| TPM state persistence across service restarts and reboots | Works |
| BitLocker data volume with a TPM protector (runtime seal/unseal) | Untested; expected to work while Windows is running |
| Windows Hello enrollment | Untested; expected to work through the same PCP path |
| BitLocker setup on the OS drive | Fails: the wizard refuses with "No TCG event is available" |
| BitLocker automatic pre-boot unlock | Not supported: the UEFI boot path cannot reach a software TPM, so a recovery prompt appears |
| `Ready For Attestation` / remote attestation | Not supported: no firmware TCG event log, and the EK chain is not Microsoft-trusted |
| Firmware measurement PCRs / measured boot | Not supported: PCRs are software-implemented |
| Attestation-only IOCTLs (`Tbsi_Create_Windows_Key`, revoke flows) | Not implemented |
| Locality != 0 policies, physical-presence flows, field upgrade | Not supported |

## Requirements

- Windows 10/11 (x64).
- Visual Studio with the C++ toolchain and the WDK to build the driver;
  CMake 3.16.3 or newer and MSVC to build the service.
- OpenSSL (headers and x64 `libcrypto`) for the TPM reference implementation
  and EK certificate generation. 1.1.1 or 3.x; the build script detects both
  installation layouts.
- Microsoft's `ms-tpm-20-ref` at `..\ms-tpm-20-ref` relative to the `softtpm`
  directory.
- Test signing enabled (Secure Boot off) to run the self-signed driver

## Build

From an elevated Developer PowerShell:

```powershell
cd softtpm
.\scripts\build.ps1 -OpenSslRoot "C:\Program Files\OpenSSL-Win64"
```

The script builds the reference TPM core into `build/tpm-prefix`, configures
and builds the service into `build/service`, builds the driver with MSBuild
when the WDK is present, and builds the `SoftTpmStatus` tool. Useful switches:
`-SkipDriver` (no WDK), `-SkipTool`, `-Clean`. `-OpenSslRoot` defaults to
`$env:SOFTTPM_OPENSSL_ROOT`.

## Sign and install

```powershell
.\scripts\test-sign.ps1                       # self-sign driver, service, tool
.\scripts\install.ps1                         # install driver, register service
.\scripts\install.ps1 -DisableExistingTpm     # also disable a physical/firmware TPM
```

A physical or firmware TPM must be disabled before installing SoftTPM, because
both claim the name `\??\TPM`. `install.ps1` detects and disables one when
told to with `-DisableExistingTpm`.

If a previous version of the driver is loaded, Windows cannot replace its
`.sys` file in place. `install.ps1` stages the replacement for the next boot
and says so; the new driver is active only after that reboot.

## Verify

```powershell
.\tools\SoftTpmStatus\bin\SoftTpmStatus.exe   # Tbsi_GetDeviceInfo + EK/SRK
Get-Tpm
tpmtool getdeviceinformation
```

The end-to-end harness rebuilds, reinstalls, runs provisioning checks and
collects logs under `logs/`:

```powershell
.\scripts\run-provision-test.ps1
```

For protocol-level debugging, `.\scripts\trace.ps1` dumps service and device
state, the driver trace ring, and exercises the TPM device directly (bypassing
TBS).

## Uninstall and reset

```powershell
.\scripts\uninstall.ps1                # stop service, uninstall driver
.\scripts\uninstall.ps1 -RemoveState   # also delete TPM state and keys
```

Deleting `%ProgramData%\SoftTpm` is equivalent to replacing a physical TPM:
any BitLocker volume sealed to this TPM requires its recovery key afterwards.

## Troubleshooting

- `Get-Tpm` reports no TPM: check `sc query SoftTpmDriver` and
  `Get-Service SoftTpmService`. If `install.ps1` warned about a staged driver
  replacement, reboot.
- Service log: `%ProgramData%\SoftTpm\softtpm.log`. Harness logs:
  `logs/` under the repository root.
- Driver trace ring (unknown IOCTLs, timeouts, rejected responses, PPI calls):
  `.\scripts\trace.ps1`.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Components, boot flow, command path, design decisions |
| [docs/driver-tpm-interface.md](docs/driver-tpm-interface.md) | The TBS to `tpm.sys` interface SoftTPM implements |
| [docs/service-protocol.md](docs/service-protocol.md) | Driver to service protocol, timeouts, recovery |
| [docs/attestation-and-limits.md](docs/attestation-and-limits.md) | EK provisioning, trust model, limitations |


## Legal

The TPM engine is Microsoft's `ms-tpm-20-ref` under its own license. SoftTPM
driver, service and tool sources are provided under the MIT license.
