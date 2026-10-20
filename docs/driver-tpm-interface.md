# The TBS to tpm.sys Interface

Windows TPM Base Services (`tbs.sys`) talks to a TPM through the device
object `\Device\TPM` (symbolic link `\??\TPM`) using a fixed set of IOCTLs.
SoftTPM's driver creates its own TPM device and implements the same contract,
so `tbs.dll`, `Win32_Tpm`, `tpm.msc` and everything built on them treat
SoftTPM as a hardware TPM.

## Device object

| Property | Value |
| --- | --- |
| Device name | `\Device\SoftTpmTpm` |
| Symbolic link | `\DosDevices\TPM` (aka `\??\TPM`) |
| Device type | `0x22C0` (the TPM device type) |
| I/O type | `METHOD_BUFFERED` |
| Exclusive | No (TBS and diagnostics tools may open it concurrently) |
| Dispatch | Sequential queue |
| ACL | `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)` - SYSTEM, Administrators, and services running as built-in users |

The device is a raw control device owned by the KMDF driver; it exists while
the driver is loaded and disappears with it. The driver itself is started at
boot through a root-enumerated device installed by `scripts/install.ps1`.

## IOCTL set

tbs.sys builds every IOCTL with DeviceType `FILE_DEVICE_UNKNOWN` (0x22) and
`FILE_READ_DATA|FILE_WRITE_DATA` access. Each code maps to one Win32 TBS API
(recovered from tbs.dll and tpm.sys disassembly):

| Code | tbs.dll API | SoftTPM behavior |
| --- | --- | --- |
| `0x22C004` | `Tbsip_Cancel_Commands` | Success, no data. The SoftTPM submit path is synchronous, so there is nothing to cancel. |
| `0x22C00C` | `Tbsip_Submit_Command` | Executes a TPM 2.0 command (below). |
| `0x22C014` | `Tbsi_Physical_Presence_Command` | Synthetic PPI responder (below). |
| `0x22C018` | `Tbsi_Revoke_Attestation` | Extends PCR[12] and increments the SoftTPM event counter (below). |
| `0x22C01C` | `Tbsi_GetDeviceInfo` | Returns the 16-byte device info (below). |
| `0x22C194` | `Tbsip_Submit_Command_NonBlocking` | Same handler as 0x22C00C; tpm.sys routes both codes to the same function. |
| `0x22C198` | `Tbsip_TestMorBit` | Returns a zero DWORD (memory overwrite request not armed). |
| `0x22C19C` | `Tbsi_Create_Windows_Key` | Validates the key handle and runs provisioning (below). |
| `0x22C1A0` | `Tbsip_TestInterruptInformation` | Returns a zero DWORD (a virtual TPM exposes no interrupt data). |

## IOCTL_TPM_SUBMIT_COMMAND

```
CTL_CODE(0x22, 3, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C00C
```

| Direction | Contents |
| --- | --- |
| Input | A TPM 2.0 command. Byte 0 is NOT the TPM tag: TBS overloads it with the command priority. |
| Output | The TPM response for that command (variable length, matches the TPM response exactly). |

Priority encoding in byte 0 (the byte TBS substitutes):

| Value | Priority |
| --- | --- |
| 0x64 (100) | LOW |
| 0xC8 (200) | NORMAL |
| 0x12C (300) | HIGH |
| 0x190 (400) | SYSTEM |

The driver reads the priority from byte 0 and restores the byte to `0x80`
before enqueueing the command. This preserves both TPM tags:
`TPM_ST_NO_SESSIONS` (0x8001) and `TPM_ST_SESSIONS` (0x8002) only differ in
byte 1, which is never touched.

Semantics:

- The request is asynchronous: it stays pending until the service answers,
  the service disconnects, or the 30-second timeout expires.
- Ordering is strict priority (SYSTEM > HIGH > NORMAL > LOW), FIFO within the
  same priority. The pending queue holds up to 64 commands; overflow completes
  the request immediately with a failure response.
- Input shorter than 10 bytes or larger than 4096 bytes is rejected with
  `STATUS_INVALID_PARAMETER` / `STATUS_INVALID_BUFFER_SIZE`.
- If no service is connected, the request fails immediately with
  `STATUS_DEVICE_NOT_READY` (TBS retries).

## IOCTL_TPM_CREATE_WINDOWS_KEY

```
CTL_CODE(0x22, 0x67, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C19C
```

`Tbsi_Create_Windows_Key` sends a 4-byte key handle and expects no output.
SoftTPM accepts the two handles Windows uses and maps the call to the
provisioning action:

- `0x81000001` (SRK) and `0x81010001` (EK) are accepted.
- Any other handle is rejected with `STATUS_INVALID_PARAMETER`.
- The service runs the same idempotent `provision_run` pass used at startup,
  creating the EK/SRK and the EK certificate if needed.
- Success completes with zero output bytes; provisioning failure or a
  disconnected service is reported as an I/O error to TBS.

## IOCTL_TPM_REVOKE_ATTESTATION

```
CTL_CODE(0x22, 6, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C018
```

`Tbsi_Revoke_Attestation` has no input and no output. The service performs:

1. `TPM2_PCR_Extend` on PCR[12] with fixed SHA-256, SHA-384 and SHA-512
   digests.
2. `TPM2_NV_Increment` on the SoftTPM attestation counter at NV index
   `0x01400010` (counter type, empty index auth, platform-created).

Success requires both commands to succeed; otherwise TBS receives an I/O
error. The NV counter is SoftTPM's own monotonic counter, not the
`tpm.sys`-managed event counter, so Windows' WBCL event-count field does not
change. This is the software-TPM limitation of the revoke path.

## Private locality submit

```
CTL_CODE(0x22, 0x69, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C1A4
```

This is a SoftTPM-only test interface, not a TBS IOCTL. It carries the
locality that the standard submit IOCTL cannot express:

```c
typedef struct _SOFTTPM_TPM_LOCALITY_INPUT {
    ULONG Locality;                 /* 0-4 */
    ULONG Priority;                 /* 0x64, 0xC8, 0x12C or 0x190 */
    UCHAR Command[4096];            /* complete TPM 2.0 command */
} SOFTTPM_TPM_LOCALITY_INPUT;
```

The input length is the 8-byte header plus the TPM command. Locality values
above 4, commands shorter than 10 bytes, and commands larger than 4096 bytes
are rejected with `STATUS_INVALID_PARAMETER`. The response is the raw TPM
response, exactly like the TBS submit path. The service temporarily sets
`_plat__LocalitySet(Locality)` around `_plat__RunCommand` and restores the
previous locality afterward.

## IOCTL_TPM_GET_DEVICE_INFO

```
CTL_CODE(0x22, 7, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C01C
```

Returns a 16-byte structure describing the TPM:

| Field | Value | Meaning |
| --- | --- | --- |
| `StructVersion` | 1 | Version of this structure. |
| `TpmVersion` | 2 | TPM 2.0. |
| `TpmInterfaceType` | 3 | "Hardware" interface. SoftTPM reports the firmware interface type so the stack treats it like an on-board TPM. |
| `TpmImpRevision` | 0 | Vendor implementation revision. |

A too-small output buffer yields `STATUS_BUFFER_TOO_SMALL`.

## IOCTL_TPM_PPI_COMMAND

```
CTL_CODE(0x22, 5, METHOD_BUFFERED, FILE_READ_DATA|FILE_WRITE_DATA) = 0x22C014
```

Implements the TCG Physical Presence Interface for
`Tbsi_Physical_Presence_Command`. Real tpm.sys forwards the request to the
firmware's ACPI `_DSM` (GUID `3DDDFAA6-361B-4EB4-A424-8D10089D1653`) or, for
emulated TPMs, to a vendor TPM command, and passes whatever the platform
reports back to the caller. SoftTPM answers directly. The input holds the PPI
function index in the first DWORD followed by the operation and argument
("Arg2"/"Arg3"/"Arg4" of the specification); responses match what TPM 2.0
platform firmware returns (cf. coreboot `src/drivers/tpm/ppi.c`):

| Fn | Meaning | Input | Output |
| --- | --- | --- | --- |
| 1 | Get PPI version | - | NUL-terminated ASCII `"1.3"` (4 bytes) |
| 2 | Submit operation | operation | DWORD 0 (success) |
| 3 | Get pending operation | - | DWORD 0, pending operation, pending argument |
| 4 | Get transition action | - | DWORD 2 (reboot) |
| 5 | Get operation response | - | DWORD 0, last operation 0, response 0 (no pre-OS environment ran) |
| 7 | Submit operation 2 | operation [, argument] | DWORD 0 (success) |
| 8 | Get confirmation status | operation | DWORD 3 (allowed; physical presence is confirmed at boot) |

Function 6 (deprecated language selection) and function 0 (the ACPI query
function) are rejected with `STATUS_INVALID_PARAMETER`, matching what tpm.sys
forwards. Operations submitted through function 2/7 are recorded and reported
back through function 3. The pending operation and argument are stored as
`PpiPendingOperation` and `PpiPendingArgument` DWORDs in the driver service
key, so they survive a driver reload. SoftTPM has no pre-OS component, so a
submitted operation is persisted and reported, never actually applied.

## Failure responses

For TPM submit timeouts, disconnects, queue overflow and malformed service
answers, the driver completes the TBS request with this canonical 10-byte TPM
response instead of an I/O error. Action requests (create/revoke) have no TPM
response body and instead complete with the corresponding I/O status, such as
`STATUS_IO_TIMEOUT` or `STATUS_DEVICE_NOT_READY`:

```
80 01 00 00 00 0A 00 00 01 01
 |  |        |     |     |
 |  |        |     |     +-- response code 0x101 (TPM_RC_FAILURE)
 |  |        |     +-------- command size 10
 |  |        +-------------- response size 10
 |  +----------------------- tag 0x8001 (TPM_ST_NO_SESSIONS)
 +-------------------------- byte 0
```

This keeps the failure inside the TPM protocol where the Windows TPM stack
expects to find it.

## Diagnostics: counters and trace ring

SoftTPM keeps per-driver counters and a 128-entry ring buffer
(`SOFTTPM_TRACE_ENTRY`, see [service-protocol.md](service-protocol.md) for the
layout) so the TBS-facing surface can be observed from user mode without a
debugger:

- Unknown IOCTLs complete with `STATUS_INVALID_DEVICE_REQUEST`, increment
  `UnknownIoctlCount`, are logged with `DbgPrintEx`, and are traced with the
  IOCTL code, buffer sizes and the first 12 input bytes.
- Command timeouts are traced (the head of the timed-out command is
  captured).
- Every PPI call is traced with its status, success or failure.
- Rejected `SEND_RESPONSE` calls leave breadcrumbs in the ring (header
  validation failures and `RequestId` mismatches), which is how the
  driver/service desynchronization that once caused rare command failures was
  pinned down.

`GET_STATUS` exposes the counters; `READ_TRACE` returns the ring. The service
dumps the last 8 entries automatically when its `SEND_RESPONSE` call fails,
and `scripts/test-debug.ps1 -Mode Trace` prints the ring on demand.

## Locality

`Tbsip_Submit_Command` takes a locality parameter, but `tbs.dll` rejects any
value other than 0 before it builds the submit IOCTL, and the IOCTL input has
no locality field. Normal TBS traffic therefore executes at locality 0. The
private `0x22C1A4` IOCTL above accepts localities 0-4 and is intended for
tests and diagnostics; it is not reachable through Windows TBS or the standard
`Tbsi_*` APIs. Policies that require a non-zero locality through TBS still
cannot be satisfied; see [attestation-and-limits.md](attestation-and-limits.md).

## Lifecycle and cleanup

- On service disconnect, pending and in-flight commands are completed with the
  failure response; pending create/revoke actions complete with
  `STATUS_DEVICE_NOT_READY`.
- On device teardown, the driver releases its own tracking entries; the
  outstanding requests remain owned by WDF and are completed or canceled as
  the device queues are torn down.
- A TBS handle closing (file cleanup) releases only that file's requests; the
  device itself lives until the driver unloads.
- While a previous driver image is still loaded, Windows cannot replace the
  `.sys` file in place. `install.ps1` stages the replacement for the next boot
  and warns; `test-debug.ps1 -Mode Full` detects the staged file and prints a
  note, because TPM behavior does not change until the reboot.
