# SoftTPM

<img width="3040" height="1554" alt="" src="https://github.com/user-attachments/assets/aa9c3e63-4492-400a-9515-c3f00864cc08" />


> **A quick heads-up:** This is a hobby project, and I've leaned on AI tools
> quite a bit while building it. It works, but expect some rough edges—please
> check anything you plan to rely on.
>
> **Please don't install this on your daily-driver PC.** It's a test-signed
> kernel driver that takes over the platform TPM. It is **not Microsoft-signed**
> and only loads with test signing enabled and Secure Boot disabled. If
> something goes wrong, you could end up locked out of BitLocker volumes or
> lose your Windows Hello setup. Distributing it for normal use would require
> Microsoft attestation signing.
>
> If you find a bug, have an idea, or spot a mistake in the docs, please open
> an issue or send a pull request. Thanks for taking a look.

SoftTPM is an open-source software TPM 2.0 for Windows. It presents a TPM 2.0
device to the Windows TPM stack without TPM hardware: `tbs.sys` binds to it,
and the TBS-based Windows TPM APIs (`Win32_Tpm`, `tpm.msc`, `tpmtool`, the
Microsoft Platform Crypto Provider) treat it as an on-board TPM.

The project has two runtime components:

- `driver/` - a KMDF driver that owns `\??\TPM` (the device TBS opens) and
  implements the `tpm.sys`-compatible IOCTL surface. It forwards TPM commands
  to user mode, answers the device-information IOCTL and provides a synthetic
  Physical Presence Interface responder.
- `service/` - a LocalSystem Windows service that hosts Microsoft's TPM 2.0
  reference implementation (`ms-tpm-20-ref`), persists TPM state across
  reboots, and provisions the Endorsement Key (EK), Storage Root Key (SRK)
  and EK certificate Windows expects from a real TPM.

## How it works

```
 applications and OS features (Get-Tpm, tpm.msc, tpmtool)
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

See [docs/architecture.md](docs/architecture.md) for the boot flow, command
path and design decisions, and
[docs/attestation-and-limits.md](docs/attestation-and-limits.md) for the
trust model.

## Status

SoftTPM is built and validated on Windows 11 x64 with the test-signed driver:

- `Get-Tpm` / `Win32_Tpm` report a ready, activated, owned TPM 2.0, and
  `tpmtool getdeviceinformation` works.
- TBS command traffic, `Tbsi_GetDeviceInfo`, and EK/SRK `ReadPublic` succeed;
  keys created through the Microsoft Platform Crypto Provider are TPM-backed.
- The scheduled `Tpm-Maintenance` task completes with result 0 and stores the
  self-issued EK certificate in the Windows `EKCertStore`.
- TPM state survives service restarts and machine reboots.
- The engine exposes SHA-256, SHA-384 and SHA-512 PCR banks. A captured
  three-bank boot log is validated, replayed at every service start and
  published as the WBCL. The verified round trip uses a 92,139-byte log
  (38 measurement events plus the spec ID event): after a reboot the WBCL is
  byte-identical to `platform-log.bin`, and `tpmtool gatherlogs` completes
  without object-context errors.

## Feature support

| Feature | Status |
| --- | --- |
| TPM discovery (`Get-Tpm`, `Win32_Tpm`, `tpm.msc`, `tpmtool`) | Works |
| TPM 2.0 command traffic through TBS (`GetCapability`, `ReadPublic`, key creation) | Works |
| EK/SRK provisioning and EK certificate at NV index `0x01C00002` | Works |
| PCR banks | SHA-256, SHA-384 and SHA-512, allocated when the TPM state is manufactured |
| Windows TPM maintenance (`Tpm-Maintenance`, `EKCertStore`) | Works (result 0, `EkCertificatePresent = 1`) |
| TPM-backed keys through the Microsoft Platform Crypto Provider | Works at runtime (software root of trust) |
| Windows Hello enrollment | Untested; expected to work through the same PCP path |
| TPM state persistence across service restarts and reboots | Works |
| BitLocker data volume with a TPM protector (runtime seal/unseal) | Untested; expected to work while Windows is running |
| BitLocker on the OS drive with a TPM protector | Not supported: the system check reboots into an environment that cannot reach a software TPM. Use a password/USB startup protector. |
| BitLocker automatic pre-boot unlock | Not supported: the UEFI boot path cannot reach a software TPM |
| `Ready For Attestation` / remote attestation | The TCG log requirement is met by the published WBCL; remote attestation still fails because the EK chain is not Microsoft-trusted |
| Firmware measurement PCRs / measured boot | A captured platform log is replayed into all three PCR banks, but the values are produced by the service at startup, not by the firmware |
| Attestation-only IOCTLs (`Tbsi_Create_Windows_Key`, revoke flows) | Not implemented |
| Locality != 0 policies, physical-presence flows, field upgrade | Not supported |

The full list of limitations is in
[docs/attestation-and-limits.md](docs/attestation-and-limits.md).

## Requirements

- Windows 10/11 (x64).
- Visual Studio with the C++ toolchain and the WDK to build the driver;
  CMake 3.16.3 or newer and MSVC to build the service.
- OpenSSL (headers and x64 `libcrypto`) for the TPM reference implementation
  and EK certificate generation. 1.1.1 or 3.x; the build script detects both
  installation layouts.
- Microsoft's `ms-tpm-20-ref` checked out at `..\ms-tpm-20-ref` relative to
  the `softtpm` directory (see [docs/Install-SoftTPM.md](docs/Install-SoftTPM.md)).
- Test signing enabled and Secure Boot disabled to run the self-signed driver.

## Repository layout

| Path | Contents |
| --- | --- |
| `driver/` | KMDF driver, INF and Visual Studio project |
| `service/` | Windows service, TPM engine integration and platform interface |
| `shared/` | Driver/service protocol header |
| `tools/SoftTpmStatus/` | TBS smoke-test tool |
| `scripts/` | The five supported entry points; internal helpers live in `scripts/lib/` |
| `docs/` | Architecture, interfaces, limitations and install guides |

## Supported scripts

Only these five entry points are supported. Internal helpers under
`scripts/lib/` are implementation details and may change.

| Script | Purpose |
| --- | --- |
| `scripts/build.ps1` | Builds `ms-tpm-20-ref`, the service, the driver (unless `-SkipDriver`) and `SoftTpmStatus` |
| `scripts/sign.ps1` | Creates or reuses a self-signed test certificate and signs the driver, service and tool |
| `scripts/install.ps1` | Installs the driver package and registers/starts `SoftTpmService` |
| `scripts/uninstall.ps1` | Stops the service, removes the device and driver package; `-RemoveState` also destroys TPM state |
| `scripts/test-debug.ps1` | Test and diagnostics entry point (`-Mode Full`, `Capture`, `Inspect`, `Validate`, `Trace`, `Status`, `Session`) |

## Documentation

| Document | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Components, boot flow, command path, design decisions |
| [docs/driver-tpm-interface.md](docs/driver-tpm-interface.md) | The TBS to `tpm.sys` interface SoftTPM implements |
| [docs/service-protocol.md](docs/service-protocol.md) | Driver to service protocol, timeouts, recovery |
| [docs/attestation-and-limits.md](docs/attestation-and-limits.md) | EK provisioning, trust model, limitations |
| [docs/testing.md](docs/testing.md) | `test-debug.ps1` modes, logs and expected results |
| [docs/Install-SoftTPM.md](docs/Install-SoftTPM.md) | Step-by-step installation |
| [docs/references.md](docs/references.md) | Prior art used for the design and its license compatibility |

## References

SoftTPM builds on, and was informed by, these projects:

- Microsoft [`ms-tpm-20-ref`](https://github.com/microsoft/ms-tpm-20-ref) -
  the TPM 2.0 reference implementation hosted by the service.
- Intel [`vtpm-td`](https://github.com/intel/vtpm-td) - vTPM-in-TD architecture, command servicing and attestation
  flow.
- Microsoft [`TSS.MSR`](https://github.com/microsoft/TSS.MSR) - TPM 2.0
  software stack used as a protocol reference.
- [`tpm2-tss`](https://github.com/tpm2-software/tpm2-tss) and
  [`tpm2-tss-engine`](https://github.com/tpm2-software/tpm2-tss-engine) -
  Linux TPM stack references for TCTI/ESAPI behaviour.

## Legal

The TPM engine is Microsoft's `ms-tpm-20-ref`, under the BSD-2-Clause
license. SoftTPM driver, service, tool and script sources are provided under
the MIT license. Binary releases that include `SoftTpmService` must carry the
`ms-tpm-20-ref` notice and the notice for the linked OpenSSL version; see
[docs/references.md](docs/references.md).
