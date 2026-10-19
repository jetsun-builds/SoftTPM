# Attestation, Provisioning and Limits

Windows expects more from a TPM than a command engine: an Endorsement Key, a
Storage Root Key and an EK certificate. `provision_run` creates them
idempotently on first start and leaves them alone afterwards.

## What gets provisioned

| Object | Details |
| --- | --- |
| EK | RSA 2048, restricted decryption key, attributes `0x000300B2` (fixed TPM + fixed parent + sensitive data origin + restricted + decrypt + admin-with-NV). Created with `TPM2_CreatePrimary` under the endorsement hierarchy (empty password auth), then persisted with `TPM2_EvictControl` to handle `0x81010001` (the TCG EK handle Windows probes). |
| SRK | RSA 2048, restricted decryption key, attributes `0x00030472` (fixed TPM + fixed parent + sensitive data origin + restricted + decrypt). Created under the owner hierarchy and persisted to `0x81000001`, the SRK handle Windows uses. |
| EK certificate | Written to NV index `0x01C00002` (the standard TCG EK certificate index) with attributes `PPWRITE | OWNERREAD | AUTHREAD | PLATFORMCREATE`. Authorization is the platform auth (the NV index requires physical presence to define/write). The write is chunked at 1024 bytes per `TPM2_NV_Write` call. |

The EK certificate is issued by a local CA that SoftTPM generates on first
provision (`ek-ca.pem` in the state directory, RSA 2048/SHA-256). The leaf
certificate binds the EK public modulus read back from the TPM
(`TPM2_ReadPublic`).

Provisioning details worth knowing:

- The whole sequence runs inside a synthesized physical-presence window
  (`_plat__Signal_PhysicalPresenceOn` ... `Off`), which is what allows the
  platform-authorized NV define and the evict-control operations.
- It is idempotent: existing EK/SRK handles are reused, and a
  `TPM_RC_NV_DEFINED` (0x14C) on the certificate index is treated as
  "already provisioned".
- Failure of provisioning is not fatal: the service logs a warning and
  continues; Windows features that only need the TPM engine (property
  queries, `Get-Tpm`) still work, but attestation-related features see a
  TPM without an EK certificate.

Validated behavior on a live Windows 11 system: the scheduled
`Tpm-Maintenance` task reads the certificate from NV and installs it into the
Windows `EKCertStore` (the `RTCCertificate`/`EKCertStore` keys under
`HKLM\SYSTEM\CurrentControlSet\Services\TPM\WMI` report it), after which
Windows reports `EkCertificatePresent = 1` and owns the TPM. The maintenance
task completes with result 0.

## Platform log (WBCL)

Windows reads the boot-time TCG event log through `Tbsi_Get_TCG_Log`, which
returns the Windows Boot Configuration Log (WBCL) kept in the registry value
`HKLM\SYSTEM\CurrentControlSet\Control\IntegrityServices\WBCL`. A machine that
boots without a firmware TPM never gets that value, which is why
`INFORMATION_TCG_EVENT_LOG` is missing and `Ready For Attestation` reports
false. On startup the service therefore publishes a well-formed crypto-agile
TCG log: a spec ID event (`Spec ID Event03`, SHA-256, SHA-384 and SHA-512
digest banks) followed by the replayed platform events, if a captured log is
configured (below).

Without a captured log the WBCL contains only the spec ID event. That log
describes the state the software TPM actually reports: PCRs start at their
reset values, nothing measured the boot, and an empty log and the untouched
PCR banks agree with each other. Disable publishing with
`--no-publish-wbcl`; `scripts/uninstall.ps1` removes the value.

### Replaying a captured platform log (optional)

SoftTPM can replay a real TCG event log taken from this machine's firmware
(or firmware vTPM) and publish it instead of the spec-ID-only log:

1. While the firmware TPM is still active, run
   `scripts\test-debug.ps1 -Mode Capture`; it snapshots the WBCL from the
   registry into `platform-log.bin`.
2. Place the file at `%ProgramData%\SoftTpm\platform-log.bin` (or point at it
   with `--platform-log=PATH`) and restart the service.

`scripts\test-debug.ps1 -Mode Full` performs both phases automatically: after
the baseline WBCL checks it preserves the `SRTMBoot.dat` captured by
`tpmtool gatherlogs`, installs it as `platform-log.bin`, restarts the service,
and checks that the published WBCL matches the captured log byte for byte.

At startup the service validates the log (`Spec ID Event03`, PCR 0,
`EV_NO_ACTION`, zero header digest, consistent event sizes), replays every
measurement event into the PCR banks the log and the TPM share by running
`TPM2_PCR_Extend` internally, and publishes the log unchanged as the WBCL.
Details:

- The TPM maintains SHA-256, SHA-384 and SHA-512 PCR banks. A log whose spec
  ID event declares any other digest algorithm (for example SHA-1 or SM3) is
  rejected; banks without digests in the log keep their reset values.
- The active bank set is fixed when the TPM state is manufactured. A state
  created by an older build keeps its original banks after this change; run
  `scripts\uninstall.ps1 -RemoveState` and reinstall (which deletes
  `%ProgramData%\SoftTpm`) to expose the SHA-512 bank. Replay still succeeds
  either way, because `TPM2_PCR_Extend` ignores digests for banks that are
  not allocated.
- Events for PCR 17-22 (DRTM) are rejected: SoftTPM runs at locality 0.
- An invalid or unreadable file is ignored with a warning, and the
  spec-ID-only log is published instead.
- The round trip is verified end to end with a 92,139-byte log containing 38
  measurement events. After rebooting with the log installed, the service
  loads and replays it again, the post-boot WBCL is byte-identical to
  `platform-log.bin`, and `scripts\test-debug.ps1 -Mode Validate` passes its
  service, WBCL, inspection, `tpmtool gatherlogs` and attestation-readiness
  checks.
- The replayed PCRs and the published log agree with each other, but both are
  produced by the service at startup, not by the firmware: a verifier that
  replays the log gets self-consistent values, not proof that the machine
  actually booted that way.

## Trust model

SoftTPM is a software TPM, and its trust properties differ from a hardware
TPM in ways that matter:

- **No external trust anchor.** The EK certificate chains to a CA whose
  private key is a local file (`ek-ca.pem`, not encrypted). Remote
  attestation verifiers will reject this chain. For local testing you can
  import `ek-ca.pem` into the machine/user trust store to make local
  verifiers accept it, but nothing outside this machine should trust that
  chain.
- **The root of trust is the OS.** `tpm.state` is AES-256-GCM protected by a
  key wrapped with DPAPI machine scope, and the files live under
  `%ProgramData%\SoftTpm` with its ACLs. An attacker with administrator or
  SYSTEM access on this machine can decrypt the state, copy it, or mint new
  EK certificates - exactly what a hardware TPM is designed to prevent.
  Treat sealed secrets as protected against other users and casual disk
  access, not against a compromised OS.
- **Cloning/rollback.** The state can be copied as a snapshot and restored
  later; there is no monotonic counter that an external party checks. A
  restored snapshot can roll the TPM back in time.
- **Physical presence is simulated.** Provisioning toggles
  `_plat__Signal_PhysicalPresenceOn/Off` internally, and the PPI responder
  answers synthetic values. No real human confirmation exists, and PP-based
  protections are only as strong as the service's own logic.

## Consequences for Windows features

| Feature | Behavior with SoftTPM |
| --- | --- |
| TPM property queries, `Get-Tpm`, `tpm.msc` | Work. TBS sees a TPM 2.0 with the standard handles and certificate index. |
| BitLocker data-volume TPM protector (sealing while Windows runs) | Seals and unseals through TBS while Windows is up; automatic unlock still requires a protected OS drive. |
| BitLocker on the OS drive with a TPM protector | Not supported. The TCG-log check is satisfied by the published platform log, but BitLocker's system check reboots and verifies that the boot environment can use the TPM; the pre-boot environment cannot reach a software TPM, so enabling with a TPM protector fails. Use a password/USB startup protector (with "Allow BitLocker without a compatible TPM") or a hardware TPM. |
| Windows TPM maintenance (`Tpm-Maintenance`) | Completes with status 0 and stores the self-issued EK certificate in the Windows `EKCertStore`. With the platform log published, the `INFORMATION_TCG_EVENT_LOG` requirement is met; remote attestation still fails on the EK chain. |
| BitLocker automatic pre-boot unlock | Does **not** work: the firmware/UEFI boot path cannot reach a software TPM. Expect a recovery prompt unless a password or recovery protector is also configured. Treat automatic unlock as unsupported. |
| Windows Hello (TPM-backed keys) | Uses TBS at runtime; expected to work, but untested in the validated matrix. The software-root-of-trust caveat applies. |
| Remote attestation (MDM, fleet verification) | Fails: the EK certificate chain is not Microsoft-trusted. |
| Measured boot / PCR-based policies | PCRs exist and extend; with a captured platform log they are replayed at startup, but the values still come from the service, not from the boot firmware. |

Running a test-signed driver requires Secure Boot to be off; for production
the driver needs Microsoft attestation signing. See the README and
[Install-SoftTPM.md](Install-SoftTPM.md) for the build and install
flow.

## Known limitations

- **Locality 0 only.** The TBS submit interface carries no locality, and the
  driver sets `Locality = 0` for every command. Policies that require a
  non-zero locality cannot be satisfied.
- **One command outstanding.** The driver delivers commands strictly one at
  a time; the queue depth is 64. Very bursty command sources see
  serialization, and a stuck command blocks others until the 30-second
  timeout.
- **NV memory is 32 KiB** (`NV_MEMORY_SIZE = 32768` in the patched
  `TpmConfiguration`). Objects and certificates must fit.
- **Fixed transient object and session limits.** The profile provides 8
  transient object slots (raised from the reference default of 3 so the
  Windows TPM stack can keep the SRK, EK and certificate objects loaded) and
  3 loaded sessions. These are compile-time constants; they are not part of
  `tpm.state` and do not change when the state is re-manufactured. The
  previous default of 3 produced `TPM_RC_OBJECT_MEMORY` (`0x80280902`) during
  `tpmtool` EK operations.
- **OS-drive BitLocker with a TPM protector cannot be enabled.** Enabling it
  runs a system check that reboots into the pre-boot environment to verify TPM
  access; the SoftTPM driver exists only in Windows, so the check fails. A
  password or USB startup-key protector avoids the TPM at boot; data volumes
  can use a TPM protector once Windows is running.
- **No field upgrade.** The reference implementation's firmware upgrade
  commands are not wired to anything; `TPM2_FieldUpgradeData` flows are
  unsupported.
- **Startup is always CLEAR.** No `Startup(STATE)` continuity across service
  restarts; a restart re-initializes volatile state (loaded keys/sessions
  are lost, NV data is not).
- **No locality, no PP attestation, no hardware RNG.** Entropy comes from
  the OS CNG random source.
- **The published platform log has no measurements by default.** The
  spec-ID-only WBCL satisfies Windows' "a TCG log exists" checks. A captured
  platform log is replayed at startup, but that is a replay, not evidence
  that the running firmware booted that way, and it disappears when
  `scripts\uninstall.ps1` runs or when firmware writes a real log on a machine
  with the previous TPM re-enabled.
- **Not certified.** No TCG, FIPS or Common Criteria certification; do not
  use for production security requirements.
- **Removing the state is irreversible for sealed data.** Deleting
  `%ProgramData%\SoftTpm` (or running
  `scripts\uninstall.ps1 -RemoveState`) is equivalent to replacing the TPM
  chip: BitLocker volumes sealed to it need their recovery keys, and Windows
  Hello enrollment is lost.

## Related documents

- [architecture.md](architecture.md) - component and data flow overview.
- [driver-tpm-interface.md](driver-tpm-interface.md) - the TBS-facing
  interface.
- [service-protocol.md](service-protocol.md) - driver/service protocol.
- [testing.md](testing.md) - replay capture, inspection and validation.
