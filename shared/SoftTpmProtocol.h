#ifndef SOFTTPM_PROTOCOL_H
#define SOFTTPM_PROTOCOL_H

/*
 * Driver <-> service protocol for SoftTPM.
 *
 * Include order requirements:
 *   kernel mode: include <ntddk.h> or <wdf.h> first
 *   user mode:   include <windows.h> first
 * CTL_CODE and the ULONG/UCHAR types come from those headers.
 */

#define SOFTTPM_PROTOCOL_VERSION 1
#define SOFTTPM_MAX_PACKET_SIZE 4096

#define SOFTTPM_CONTROL_DEVICE_NAME L"\\Device\\SoftTpmControl"
#define SOFTTPM_CONTROL_LINK_NAME L"\\DosDevices\\SoftTpmControl"
#define SOFTTPM_CONTROL_WIN32_NAME L"\\\\.\\SoftTpmControl"

#define SOFTTPM_TPM_DEVICE_NAME L"\\Device\\SoftTpmTpm"
#define SOFTTPM_TPM_LINK_NAME L"\\DosDevices\\TPM"

#define SOFTTPM_POOL_TAG 'sTpm'

#define SOFTTPM_COMMAND_TIMEOUT_MS 30000
#define SOFTTPM_MAX_PENDING_COMMANDS 64
#define SOFTTPM_TRACE_ENTRY_COUNT 128
#define SOFTTPM_TRACE_DATA_SIZE 12

#define SOFTTPM_DEVICE_TYPE 0x22C0

#define IOCTL_SOFTTPM_GET_COMMAND \
    CTL_CODE(SOFTTPM_DEVICE_TYPE, 0x900, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_SOFTTPM_SEND_RESPONSE \
    CTL_CODE(SOFTTPM_DEVICE_TYPE, 0x901, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_SOFTTPM_GET_STATUS \
    CTL_CODE(SOFTTPM_DEVICE_TYPE, 0x902, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_SOFTTPM_READ_TRACE \
    CTL_CODE(SOFTTPM_DEVICE_TYPE, 0x903, METHOD_BUFFERED, FILE_READ_DATA)

#define SOFTTPM_STATE_UNKNOWN 0
#define SOFTTPM_STATE_STOPPED 1
#define SOFTTPM_STATE_RUNNING 2

typedef struct _SOFTTPM_COMMAND {
    ULONG ProtocolVersion;
    ULONG RequestId;
    ULONG Locality;
    ULONG CommandSize;
    UCHAR Command[SOFTTPM_MAX_PACKET_SIZE];
} SOFTTPM_COMMAND, *PSOFTTPM_COMMAND;

typedef struct _SOFTTPM_RESPONSE {
    ULONG ProtocolVersion;
    ULONG RequestId;
    ULONG Status;
    ULONG ResponseSize;
    UCHAR Response[SOFTTPM_MAX_PACKET_SIZE];
} SOFTTPM_RESPONSE, *PSOFTTPM_RESPONSE;

typedef struct _SOFTTPM_STATUS {
    ULONG ProtocolVersion;
    ULONG State;
    ULONG PendingCommandCount;
    ULONG CompletedCommandCount;
    ULONG TimeoutCount;
    ULONG UnknownIoctlCount;
    ULONG LastTpmResponseCode;
} SOFTTPM_STATUS, *PSOFTTPM_STATUS;

typedef struct _SOFTTPM_TRACE_ENTRY {
    ULONG Sequence;
    ULONG IoControlCode;
    ULONG InputSize;
    ULONG OutputSize;
    ULONG Status;
    UCHAR Data[SOFTTPM_TRACE_DATA_SIZE];
} SOFTTPM_TRACE_ENTRY, *PSOFTTPM_TRACE_ENTRY;

#endif
