# SoftTPM Architecture

SoftTPM presents a TPM 2.0 that Windows talks to exactly like a hardware TPM.
The kernel side answers the device interface `tbs.sys` expects; the user-mode
side hosts Microsoft's reference implementation and owns the persistent
state.

## Component overview

```
 tpm.msc, Win32_Tpm, tpmtool, ...
        |
        v
 tbs.dll  ->  tbs.sys  (Windows TPM Base Services, kernel)
        |              IOCTL_TPM_SUBMIT_COMMAND
        |              IOCTL_TPM_GET_DEVICE_INFO
        v
 SoftTpmDriver (KMDF)
        |  \Device\SoftTpmTpm      ->  \??\TPM          (what TBS opens)
        |  \Device\SoftTpmControl  ->  \??\SoftTpmControl
        |
        |  driver <-> service protocol (shared/SoftTpmProtocol.h)
        v
 SoftTpmService (user mode, LocalSystem)
        |  engine.c      ms-tpm-20-ref TPM 2.0 engine (static lib)
        |  platform/     _plat__* platform interface for the engine
        |  provision/    EK, SRK and EK certificate provisioning
        |  storage/      encrypted persistent state
        v
 %ProgramData%\SoftTpm\  (state.key, tpm.state, ek-ca.pem, softtpm.log)
```

## Components

| Path | Role |
| --- | --- |
| `driver/` | KMDF driver. Creates the two control devices, owns the priority queue, the 30-second timeout timer and the trace ring. Implements the `tpm.sys`-compatible IOCTL surface. |
| `service/src/engine.c` | Links the reference TPM core (`Tpm_CoreLib`, built from `ms-tpm-20-ref`) and drives its lifecycle: load state, manufacture if needed, power on, `TPM2_Startup`, command execution, shutdown. |
| `service/src/platform/` | The reference implementation's platform interface (`_plat__*`): NV memory backed by the state store, time, PCR store, ACT store, entropy from CNG, physical-presence signals, cancellation, and the Windows platform log: it loads and validates a captured TCG event log, replays it into the PCRs, and publishes it (or a spec-ID-only log) as the WBCL for readiness checks. |
| `service/src/provision/` | Creates the EK and SRK (RSA 2048), persists them to the standard handles, and writes an EK certificate to the standard NV index. |
| `service/src/storage/` | Encrypted state persistence: `state.key` (DPAPI machine scope) and `tpm.state` (AES-256-GCM), atomic replace. |
| `service/src/driver_client.c` | The service side of the driver protocol: opens the control device, serves commands until stopped, reconnects if the driver goes away, dumps the driver trace ring when a response is rejected. |
| `shared/SoftTpmProtocol.h` | The single contract between driver and service; included by both. |
| `tools/SoftTpmStatus/` | Smoke-test tool: uses TBS (`Tbsi_*`) and prints what the Windows stack sees (device info, TPM properties, EK/SRK public areas). |
| `scripts/` | Five entry points: `build.ps1`, `sign.ps1`, `install.ps1`, `uninstall.ps1` and `test-debug.ps1`. The test/diagnostic implementation is in `scripts/lib/`. |
| `docs/` | Architecture, TBS and driver/service interfaces, trust model, testing and real-machine install guide. |

## Boot and startup flow

1. `install.ps1` creates a root-enumerated device (`Root\SoftTpm`) bound to
   `SoftTpmDriver`, so the driver loads at boot. `DriverEntry` creates both
   control devices (`\??\TPM` and `\??\SoftTpmControl`); the PnP device node
   itself carries no I/O role.
2. TBS finds `\??\TPM` and starts reporting a TPM. Commands that arrive before
   the service connects get `STATUS_DEVICE_NOT_READY` and are retried by the
   Windows stack.
3. The service starts (SCM), opens `\\.\SoftTpmControl` and issues its first
   `GET_COMMAND`, which claims the connection. Closing that handle (crash,
   stop) flushes all pending TPM requests with a synthesized failure response.
4. `engine_start`: opens the state store, enables NV memory (loading
   `tpm.state`, manufacturing the TPM on first run), initializes ACTs, signals
   power-on and reset, then runs `TPM2_Startup(CLEAR)`.
5. `platform_log_load_and_replay`: if a captured platform TCG log is present
   (`platform-log.bin` in the state directory or `--platform-log=PATH`), it is
   validated and every measurement event is replayed into the PCRs with
   internal `TPM2_PCR_Extend` commands, one extend per digest the log
   declares. Banks the log does not cover keep their reset values.
6. `provision_run`: if the EK/SRK/EK certificate are not present yet, they are
   created and persisted (idempotent, wrapped in a physical-presence window
   for the platform-authorized operations), then `platform_wbcl_publish`
   publishes the platform log (or the spec-ID-only log) as the WBCL registry
   value.
7. The service serves commands in a loop (see below) until asked to stop. On
   stop it runs `TPM2_Shutdown(CLEAR)`, signals power-off and exits.

## TPM command path

1. `tbs.sys` opens `\??\TPM` and submits `IOCTL_TPM_SUBMIT_COMMAND`; the first
   byte of the input buffer carries the command priority, not the TPM tag
   (TBS replaces the tag's high byte with the priority).
2. The driver restores the byte to `0x80` (every TPM 2.0 command tag starts
   with `0x80`) and enqueues the command. Ordering is strict priority
   (`SYSTEM > HIGH > NORMAL > LOW` by the TBS priority byte), FIFO within one
   priority; the queue holds up to 64 commands.
3. The service's outstanding `IOCTL_SOFTTPM_GET_COMMAND` completes with the
   next command and a driver-assigned `RequestId`. The driver marks the
   command as delivered *before* completing that request, so an immediate
   `SEND_RESPONSE` can never be misjudged as stale.
4. The service executes the command on the reference engine and answers with
   `IOCTL_SOFTTPM_SEND_RESPONSE`, echoing the `RequestId`.
5. The driver validates the answer against the delivered command and completes
   the original TBS request with the TPM response bytes.

If the service is not connected, the answer is malformed, or the 30-second
timeout expires, the TBS request is completed with a synthesized TPM failure
response (a well-formed 10-byte TPM response, so callers see a TPM-level
failure, not an I/O error).

## Design decisions

- **Separate control device.** TBS traffic and service traffic never share a
  device object. The service connects to `\\.\SoftTpmControl`; TBS sees only
  `\??\TPM`. Service presence is exactly "control device open".
- **Reference engine in the service process.** The service statically links
  `ms-tpm-20-ref` and hosts it directly; no extra process, RPC or COM. The
  engine's platform interface is implemented with ordinary Windows APIs.
- **One command outstanding.** The driver delivers one command at a time and
  matches the response by `RequestId`. This mirrors the serialized nature of
  TPM command execution and keeps the state model simple. Stale or duplicate
  responses are rejected and leave a breadcrumb in the trace ring.
- **Failure responses instead of I/O errors.** Upper layers (TBS, the TPM
  service, BitLocker) handle TPM-level failures better than driver I/O
  failures, so the driver synthesizes a canonical failure response
  (`TPM_RC_FAILURE`) for timeouts, disconnects and queue overflow.
- **Trace ring and counters.** The full `tpm.sys` IOCTL surface is not
  publicly documented. Unknown IOCTLs, command timeouts, PPI calls and
  rejected responses are counted and recorded so the real surface can be
  discovered from a live machine; `READ_TRACE` and `GET_STATUS` expose the
  ring and counters to the service and diagnostics tools.
- **Encrypted state.** `tpm.state` is AES-256-GCM encrypted with a key that is
  itself wrapped by DPAPI machine scope. Copies of the state file are useless
  without the machine context. See
  [attestation-and-limits.md](attestation-and-limits.md) for the trust model.

## State persistence

| File | Contents |
| --- | --- |
| `%ProgramData%\SoftTpm\state.key` | 32-byte AES key, DPAPI machine scope, entropy mixed with the machine GUID. |
| `%ProgramData%\SoftTpm\tpm.state` | NV memory: 48-byte header (magic `SFTSTATE`, version, flags, size, sequence, nonce, tag) followed by AES-256-GCM ciphertext. Written with a temp file + atomic replace. |
| `%ProgramData%\SoftTpm\ek-ca.pem` | Local CA used to issue the EK certificate (see attestation docs). |
| `%ProgramData%\SoftTpm\platform-log.bin` | Optional captured TCG event log replayed at startup. |
| `%ProgramData%\SoftTpm\softtpm.log` | Service log. |

The NV image is 32 KiB (`NV_MEMORY_SIZE` in the patched `TpmConfiguration`).
The patch also raises `MAX_LOADED_OBJECTS` from the reference default of 3 to
8 so the Windows TPM stack (`tpmtool gatherlogs`, TBS) can keep the SRK, EK
and certificate objects loaded at once. Transient objects live only in RAM and
are not part of `tpm.state`.

The prepared profile enables SHA-512 and allocates SHA-256, SHA-384 and
SHA-512 PCR banks when the TPM is manufactured; a state created by an older
build keeps its original bank set until it is re-manufactured. Deleting the
state directory is equivalent to replacing the TPM chip: all sealed data
(BitLocker, Windows Hello) requires recovery.

## Tooling

The five supported entry points are build, sign, install, uninstall and
test-debug. `test-debug.ps1` provides the end-to-end harness (`-Mode Full`),
platform-log capture/inspection (`Capture`, `Inspect`), post-reboot validation
(`Validate`), driver tracing (`Trace`), a quick status snapshot (`Status`) and
an interactive session (`Session`). See [testing.md](testing.md).

## Related documents

- [driver-tpm-interface.md](driver-tpm-interface.md) - the TBS-facing
  interface in detail.
- [service-protocol.md](service-protocol.md) - the driver/service protocol,
  timeouts and recovery.
- [attestation-and-limits.md](attestation-and-limits.md) - provisioning,
  trust model and known limitations.
- [Install-SoftTPM.md](Install-SoftTPM.md) - installation guide.
