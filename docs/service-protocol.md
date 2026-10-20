# Driver to Service Protocol

The contract between `SoftTpmDriver` and `SoftTpmService` lives in one
header, [`shared/SoftTpmProtocol.h`](../shared/SoftTpmProtocol.h), included
by both sides. Protocol version: **2**. A protocol change requires rebuilding
and reinstalling both the driver and the service; the harness needs
`-ReinstallDriver` (plus a reboot if the old image is still loaded).

## Device and connection model

| Name | Value |
| --- | --- |
| Device | `\Device\SoftTpmControl` |
| Symbolic link | `\DosDevices\SoftTpmControl` |
| Win32 path | `\\.\SoftTpmControl` |
| Pool tag | `sTpm` |

The control device has a stricter ACL than `\??\TPM`: SYSTEM and
Administrators only (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`).

"Connected" means the service holds an open handle to
`\\.\SoftTpmControl`; the driver tracks it via file create/cleanup callbacks.
There is no login, handshake or version negotiation IOCTL - the service just
opens the device and starts the loop. The connection is bound to the first
file object that issues `GET_COMMAND`; that file object *is* the service.
Diagnostics clients that only read `GET_STATUS`/`READ_TRACE` open and close
freely without affecting the service link.

- **Open** -> `ServiceConnected = TRUE`; TBS commands may now be served.
- **Close** (service crash or stop) -> all in-flight and pending TBS requests
  are completed with the failure response, the TBS queue is purged and
  restarted, and new TBS commands fail with `STATUS_DEVICE_NOT_READY` until
  the service returns.

## Common framing

All control IOCTLs use `METHOD_BUFFERED` and device type `0x22C0`. Both
directions validate `ProtocolVersion` and reject mismatches with
`STATUS_INVALID_PARAMETER`.

```c
typedef struct _SOFTTPM_COMMAND {
    ULONG ProtocolVersion;                  /* SOFTTPM_PROTOCOL_VERSION */
    ULONG RequestId;                        /* driver-assigned, per command */
    ULONG Action;                           /* submit / create key / revoke */
    ULONG Locality;                         /* 0-4; TBS uses 0 */
    ULONG CommandSize;                      /* 0 for non-submit actions */
    UCHAR Command[SOFTTPM_MAX_PACKET_SIZE]; /* 4096 */
} SOFTTPM_COMMAND;

#define SOFTTPM_ACTION_SUBMIT              0
#define SOFTTPM_ACTION_CREATE_WINDOWS_KEY  1
#define SOFTTPM_ACTION_REVOKE_ATTESTATION  2

typedef struct _SOFTTPM_RESPONSE {
    ULONG ProtocolVersion;
    ULONG RequestId;                        /* echo of the command */
    ULONG Status;                           /* 0 = Response valid */
    ULONG ResponseSize;
    UCHAR Response[SOFTTPM_MAX_PACKET_SIZE];
} SOFTTPM_RESPONSE;
```

## IOCTLs

| IOCTL | Code | Direction |
| --- | --- | --- |
| `IOCTL_SOFTTPM_GET_COMMAND` | 0x22C0E400 | Output: `SOFTTPM_COMMAND` |
| `IOCTL_SOFTTPM_SEND_RESPONSE` | 0x22C0E404 | Input: `SOFTTPM_RESPONSE` |
| `IOCTL_SOFTTPM_GET_STATUS` | 0x22C06408 | Output: `SOFTTPM_STATUS` |
| `IOCTL_SOFTTPM_READ_TRACE` | 0x22C0640C | Output: `SOFTTPM_TRACE_ENTRY[]` |

(These are the `CTL_CODE` expressions from the header; use the macros, not
the raw values.)

### GET_COMMAND

Blocks until a TPM command or action is available, then completes with the
command payload (`FIELD_OFFSET(..., Command) + CommandSize` bytes). The driver
hands out the highest-priority item first (the TBS priority byte ordering
described in
[driver-tpm-interface.md](driver-tpm-interface.md)), and byte 0 of a delivered
TPM command is already restored to `0x80`. Non-submit actions have
`CommandSize == 0`; their `Action` and `Locality` fields are set by the
driver.

One command is outstanding system-wide: the driver delivers the head of the
priority queue and does not deliver another until it is answered, times out,
or the service disconnects. A command is marked *delivered* before the
`GET_COMMAND` request completes, so a response that arrives immediately after
the completion is still accepted; without that ordering, a fast service could
have its answer misjudged as stale and the connection torn down.

The service issues `GET_COMMAND` as overlapped I/O and waits on both the
driver event and its stop event, so shutdown never hangs on a pending
request. A `GET_COMMAND` from any file object other than the claimed service
fails with `STATUS_DEVICE_BUSY`. When the service file object is closed, the
driver drains pending `GET_COMMAND` requests, fails the in-flight TBS
commands, and reopens the queue for the next connection.

### SEND_RESPONSE

Validated in two stages.

Header validation - failure completes with `STATUS_INVALID_PARAMETER` and
leaves a trace breadcrumb (`0xFFFF` for an unreadable header, `0xFFFE` for a
bad header payload):

- input shorter than the fixed header, or
- `ProtocolVersion` mismatch, or
- `ResponseSize > 4096`, or
- input shorter than header + `ResponseSize`.

Command matching - the driver only accepts a response when there is a
delivered, not-yet-completed current command and the `RequestId` matches.
Anything else (stale, duplicate, unknown) completes with
`STATUS_INVALID_PARAMETER` and writes an `'RJCT'` breadcrumb (the current and
received `RequestId`, plus delivered/completed/connected flags) into the
trace ring; the TBS request stays pending and fails via the 30-second
timeout. Successful `SEND_RESPONSE` calls then branch on the pending action:

- `SOFTTPM_ACTION_SUBMIT`: `Status != 0` or `ResponseSize < 10` completes the
  TBS request with the synthesized failure response; otherwise the TBS
  request is completed with the exact response bytes. `CompletedCommandCount`
  and `LastTpmResponseCode` are updated (big-endian response code from
  offset 6).
- `SOFTTPM_ACTION_CREATE_WINDOWS_KEY` and
  `SOFTTPM_ACTION_REVOKE_ATTESTATION`: success requires `Status == 0` and
  `ResponseSize == 0`; the TBS request completes successfully with zero
  output bytes. Any other answer completes with `STATUS_UNSUCCESSFUL`.

The service dumps the last 8 trace entries automatically when a
`SEND_RESPONSE` call fails, which makes a rejection reason visible in
`softtpm.log` without an attached debugger.

### GET_STATUS

```c
typedef struct _SOFTTPM_STATUS {
    ULONG ProtocolVersion;
    ULONG State;                 /* 0 unknown, 1 stopped, 2 running */
    ULONG PendingCommandCount;   /* queued + current */
    ULONG CompletedCommandCount;
    ULONG TimeoutCount;
    ULONG UnknownIoctlCount;
    ULONG LastTpmResponseCode;
} SOFTTPM_STATUS;
```

### READ_TRACE

Returns the trace ring, oldest entry first:

```c
typedef struct _SOFTTPM_TRACE_ENTRY {
    ULONG Sequence;              /* monotonically increasing */
    ULONG IoControlCode;
    ULONG InputSize;
    ULONG OutputSize;
    ULONG Status;
    UCHAR Data[SOFTTPM_TRACE_DATA_SIZE];   /* first input bytes, zero padded */
} SOFTTPM_TRACE_ENTRY;
```

The ring holds the last 128 entries (`SOFTTPM_TRACE_ENTRY_COUNT`). Entries
are recorded for:

- unknown TBS IOCTLs (IOCTL code, buffer sizes, first 12 input bytes),
- command timeouts (IOCTL 0 with `STATUS_IO_TIMEOUT`, plus the head of the
  timed-out TPM command),
- every PPI call, with the resulting status,
- rejected `SEND_RESPONSE` calls (`0xFFFF`/`0xFFFE` header failures, `'RJCT'`
  mismatches with diagnostic values).

## Timeouts

| Constant | Value | Meaning |
| --- | --- | --- |
| `SOFTTPM_COMMAND_TIMEOUT_MS` | 30000 | Deadline for the delivered command. |
| `SOFTTPM_MAX_PENDING_COMMANDS` | 64 | Queue depth before overflow failures. |

When the timer fires and the command was never delivered (or delivery
failed), a submit request is completed with the failure response, an action
request completes with `STATUS_IO_TIMEOUT`, `TimeoutCount` is incremented, and
a trace entry is written. While a delivery is momentarily in progress the
timer re-arms itself every second instead of failing the command, so a slow
`GET_COMMAND` completion cannot produce a spurious timeout.

## Service behavior

`driver_client.c` loop:

1. `CreateFileW("\\.\SoftTpmControl", ..., FILE_FLAG_OVERLAPPED)`.
2. Serve commands until the device errors, the service stop event is
   signaled, or a malformed payload arrives.
3. On failure: close the handle (driver flushes pending work) and retry
   every 1000 ms. The first failure and every 30th are logged.

For `SOFTTPM_ACTION_SUBMIT`, the service calls
`engine_execute_locality(command, size, Locality, ...)`; locality 0 is the
normal TBS path. `engine_execute` returning non-zero (engine not running,
truncated command) is answered with `Status = 1` and an empty response, which
the driver converts into the TPM failure response for TBS.

For `SOFTTPM_ACTION_CREATE_WINDOWS_KEY`, the service runs the idempotent
provisioning pass and answers with `Status = 0` and no data. For
`SOFTTPM_ACTION_REVOKE_ATTESTATION`, it runs the PCR[12] extend plus event
counter increment and answers the same way. Unknown actions and actions with
a non-zero `CommandSize` are rejected with `Status = 1`.

## Recovery scenarios

| Scenario | Behavior |
| --- | --- |
| Service crash/restart | Pending TBS commands fail (well-formed TPM failure). Service reconnects within ~1 s, reloads `tpm.state`, and runs `TPM2_Startup(CLEAR)` again. Sealed data is unaffected because the state is persisted. |
| Driver restart (unload/reload) | Service loses the control handle, retries until the device is back, reconnects. |
| Both restart | Same as boot flow; TBS retries `STATUS_DEVICE_NOT_READY` until the service answers. |
| Machine reboot | State loads from disk; every boot starts a fresh `TPM2_Startup(CLEAR)` (no saved/shutdown state is preserved beyond the NV image). |

## Diagnosing protocol failures

- `scripts/test-debug.ps1 -Mode Trace` prints `GET_STATUS` counters and the
  driver trace ring, then performs a direct driver round trip bypassing TBS.
- A rejected response leaves `'RJCT'` (or a header error) in the ring; the
  service automatically logs the last 8 entries in `softtpm.log`.
- The status snapshot (`-Mode Status`) shows service state and the tail of
  the service log.
