# References and Prior Art

SoftTPM is original code, but its design follows the TCG specifications and
draws on several upstream projects. This page records what each source was
used for and what its license allows. The local checkouts are not part of this
repository.

## Reference checkouts

| Source |  Used for |
| --- | --- |
| Intel `vtpm-td` | vTPM-in-TD architecture, command servicing, TDX attestation flow and the split between a vTPM service and the guest TPM interface. |
| Microsoft `ms-tpm-20-ref` |  The TPM 2.0 engine hosted by `SoftTpmService`; the `_plat__*` platform interface implemented under `service/src/platform/`. |
| Microsoft `TSS.MSR` | TPM command/response structures, TBS usage patterns and TPM 2.0 protocol details. |
| `tpm2-tss` | TCTI/ESAPI behaviour and TPM command marshalling references. |
| `tpm2-tss-engine` | TPM-backed key and engine integration patterns. |

## Specifications

- TCG TPM 2.0 Library Specification (commands, structures, PCR and NV
  semantics).
- TCG PC Client Platform Firmware Profile (TCG2 event log format, PCR
  allocation, `Spec ID Event03`).
- TCG Physical Presence Interface Specification (PPI function index and
  response layout).
- Microsoft TBS documentation and observations from `tbs.dll` / `tpm.sys`
  (the IOCTL surface implemented by the driver).

## How the references map to SoftTPM

| SoftTPM area | Reference material |
| --- | --- |
| Platform log validation and replay (`platform_log.c`) | TCG2 event-log layout; vTPM-attestation read-only reference for event/PCR ideas (no code copied) |
| WBCL publishing (`platform_wbcl.c`) | Windows `IntegrityServices\WBCL` format; TCG crypto-agile log rules |
| EK/SRK/EK certificate provisioning | TCG EK credential guidance; `ms-tpm-20-ref` commands |
| Driver/service protocol | Local design; TBS IOCTL behaviour recovered from `tbs.dll`/`tpm.sys` |
| Attestation and quote flow | `vtpm-td` flow ideas; vTPM-attestation read-only reference (no code copied), adapted to Windows TBS |
| TPM-backed keys | TSS.MSR and `tpm2-tss-engine` usage patterns |

See [attestation-and-limits.md](attestation-and-limits.md) for what these
references do **not** provide: a hardware trust anchor, a Microsoft-trusted
EK chain, or pre-boot TPM access.
