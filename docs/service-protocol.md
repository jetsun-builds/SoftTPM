# Driver to Service Protocol

The contract between `SoftTpmDriver` and `SoftTpmService` lives in one
header, [`shared/SoftTpmProtocol.h`](../shared/SoftTpmProtocol.h), included
by both sides. Protocol version: **1**.

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
    ULONG Locality;                         /* always 0 */
    ULONG CommandSize;
    UCHAR Command[SOFTTPM_MAX_PACKET_SIZE]; /* 4096 */
} SOFTTPM_COMMAND;

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

Blocks until a TPM command is available, then completes with the command
payload (`FIELD_OFFSET(..., Command) + CommandSize` bytes). The driver hands
out the highest-priority command first (the TBS priority byte ordering
described in
[driver-tpm-interface.md](driver-tpm-interface.md)), and byte 0 of the
delivered buffer is already restored to `0x80`.

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
timeout. Successful `SEND_RESPONSE` calls then:

- `Status != 0` or `ResponseSize < 10` -> the driver completes the TBS
  request with the synthesized failure response.
- Otherwise the TBS request is completed with the exact response bytes, and
  `CompletedCommandCount`/`LastTpmResponseCode` are updated (big-endian
  response code from offset 6).

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
failed), the TBS request is completed with the failure response,
`TimeoutCount` is incremented, and a trace entry is written. While a
delivery is momentarily in progress the timer re-arms itself every second
instead of failing the command, so a slow `GET_COMMAND` completion cannot
produce a spurious timeout.

## Service behavior

`driver_client.c` loop:

1. `CreateFileW("\\.\SoftTpmControl", ..., FILE_FLAG_OVERLAPPED)`.
2. Serve commands until the device errors, the service stop event is
   signaled, or a malformed payload arrives.
3. On failure: close the handle (driver flushes pending work) and retry
   every 1000 ms. The first failure and every 30th are logged.

`engine_execute` returning non-zero (engine not running, truncated command)
is answered with `Status = 1` and an empty response; the driver converts that
into the TPM failure response for TBS.

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
