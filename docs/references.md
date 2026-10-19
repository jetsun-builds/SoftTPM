# References and Prior Art

SoftTPM is original code, but its design follows the TCG specifications and
draws on several upstream projects. This page records what each source was
used for and what its license allows. The local checkouts are not part of this
repository.

## Local reference checkouts

| Source | Location | Used for |
| --- | --- | --- |
| Intel `vtpm-td` | `..\vtpm-td\` | vTPM-in-TD architecture, command servicing, TDX attestation flow and the split between a vTPM service and the guest TPM interface. |
| `vTPM-attestation` (hecmay/UEFI sample) | Not vendored; upstream `hecmay/vTPM-attestation` | Read-only reference for event-log, PCR and UEFI attestation ideas; no code copied. |
| Microsoft `ms-tpm-20-ref` | `..\ms-tpm-20-ref\` | The TPM 2.0 engine hosted by `SoftTpmService`; the `_plat__*` platform interface implemented under `service/src/platform/`. |
| Microsoft `TSS.MSR` | `..\TSS.MSR\` | TPM command/response structures, TBS usage patterns and TPM 2.0 protocol details. |
| `tpm2-tss` | `..\tpm2-tss\` | TCTI/ESAPI behaviour and TPM command marshalling references. |
| `tpm2-tss-engine` | `..\tpm2-tss-engine\` | TPM-backed key and engine integration patterns. |

The checkouts are not part of the SoftTPM build except `ms-tpm-20-ref`, which
is required by `scripts/build.ps1` and is statically linked into the service.

## License compatibility

This is a summary of the license files and SPDX headers found in the local
checkouts, not legal advice. "MIT-compatible" below means the license is
permissive enough to combine with MIT-licensed SoftTPM code in a combined
distribution; it does **not** mean the other code is relicensed under MIT.

| Project | License found | Compatible with MIT? | What it means |
| --- | --- | --- | --- |
| `ms-tpm-20-ref` | BSD-2-Clause (Microsoft/TCG) | Yes | Keep the copyright notice, conditions and disclaimer in source and binary distributions. This code is linked into `SoftTpmService.exe`. |
| `TSS.MSR` | MIT (root and `TSS.JS`); BSD-2-Clause (`TSS.C`) | Yes | Keep the applicable notice if code is reused. |
| `tpm2-tss` | BSD-2-Clause; a few BSD-3-Clause CI scripts; REUSE files use CC0-1.0, CC-BY-4.0 and FSFULLR | Yes | Permissive. Keep the notices for any copied code. |
| `tpm2-tss-engine` | BSD-3-Clause (file headers say BSD-2, but `LICENSE` is the 3-clause text) | Yes | Keep the notice and do not use the copyright holder's name to endorse SoftTPM. |
| `vtpm-td` (Intel) | Apache-2.0; some source files BSD-2-Clause-Patent; bundled deps mix Apache-2.0, MIT, BSD-2/3 and dual GPL-2.0/BSD-3-Clause | Yes, for the top-level source, with Apache-2.0 obligations | The Apache-2.0 files stay under Apache-2.0: include the license and any `NOTICE` file, and state changes. Prefer the BSD option for dual GPL/BSD files; do not use GPL-only code. Review `vtpm-td/deps/` separately if you reuse it. |
| `vTPM-attestation` | No top-level license file; some files carry BSD notices, and `Utility/` includes GPL/CPL components | **No** | Read-only reference. Do not copy code into SoftTPM. |

Two practical consequences:

1. **`ms-tpm-20-ref` is distributed inside SoftTPM.** Because the service
   statically links it, binary releases must include its BSD-2-Clause notice.
   Add a `THIRD_PARTY_NOTICES` file (or bundle the license) before publishing
   binaries.
2. **OpenSSL is also linked.** OpenSSL 3.x is Apache-2.0 and OpenSSL 1.1.1
   uses the OpenSSL/SSLeay license; include the applicable notice for the
   version you ship.

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
