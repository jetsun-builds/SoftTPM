# Attestation, Provisioning and Limits

Windows expects to find more on a TPM than just the engine: an Endorsement
Key, a Storage Root Key and an EK certificate. `provision_run` creates them
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
| BitLocker TPM protector (sealing while Windows runs) | Seals and unseals through TBS while Windows is up. |
| BitLocker setup on the OS drive | The wizard refuses to start: "No TCG event is available." BitLocker requires the firmware-provided TCG event log, which a driver-only software TPM cannot supply. |
| Windows TPM maintenance (`Tpm-Maintenance`) | Completes with status 0 and stores the self-issued EK certificate in the Windows `EKCertStore`. `Ready For Attestation` still reports false because the firmware TCG event log is absent (`INFORMATION_TCG_EVENT_LOG`). |
| BitLocker automatic pre-boot unlock | Does **not** work: the firmware/UEFI boot path cannot reach a software TPM. Expect a recovery prompt unless a password or recovery protector is also configured. Treat automatic unlock as unsupported. |
| Windows Hello (TPM-backed keys) | Uses TBS at runtime; works, with the same software-root-of-trust caveat. |
| Remote attestation (MDM, fleet verification) | Fails: the EK certificate chain is not Microsoft-trusted. |
| Measured boot / PCR-based policies | PCRs exist and extend, but PCR values reflect a software implementation started by the service, not firmware measurements from boot. |

Running a test-signed driver requires Secure Boot to be off; for production
the driver needs Microsoft attestation signing. See the README for the build
and install flow.

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
- **No field upgrade.** The reference implementation's firmware upgrade
  commands are not wired to anything; `TPM2_FieldUpgradeData` flows are
  unsupported.
- **Startup is always CLEAR.** No `Startup(STATE)` continuity across service
  restarts; a restart re-initializes volatile state (loaded keys/sessions
  are lost, NV data is not).
- **No locality, no PP attestation, no hardware RNG.** Entropy comes from
  the OS CNG random source.
- **Not certified.** No TCG, FIPS or Common Criteria certification; do not
  use for production security requirements.
- **Removing the state is irreversible for sealed data.** Deleting
  `%ProgramData%\SoftTpm` (or running `uninstall.ps1 -RemoveState`) is
  equivalent to replacing the TPM chip: BitLocker volumes sealed to it need
  their recovery keys, and Windows Hello enrollment is lost.

## Related documents

- [architecture.md](architecture.md) - component and data flow overview.
- [driver-tpm-interface.md](driver-tpm-interface.md) - the TBS-facing
  interface.
- [service-protocol.md](service-protocol.md) - driver/service protocol.
