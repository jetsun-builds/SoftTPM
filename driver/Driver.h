#ifndef SOFTTPM_DRIVER_H
#define SOFTTPM_DRIVER_H

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "../shared/SoftTpmProtocol.h"

#define SOFTTPM_FAILURE_RESPONSE_SIZE 10

typedef struct _SOFTTPM_PENDING_COMMAND
{
    LIST_ENTRY ListEntry;
    WDFREQUEST TbsRequest;
    ULONG RequestId;
    ULONG Priority;
    ULONG RequestLength;
    BOOLEAN Completed;
    UCHAR Request[SOFTTPM_MAX_PACKET_SIZE];
} SOFTTPM_PENDING_COMMAND, *PSOFTTPM_PENDING_COMMAND;

typedef struct _SOFTTPM_DEVICE_CONTEXT
{
    WDFDEVICE Device;
} SOFTTPM_DEVICE_CONTEXT, *PSOFTTPM_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SOFTTPM_DEVICE_CONTEXT, SoftTpmGetDeviceContext)

typedef struct _SOFTTPM_GLOBAL_STATE
{
    BOOLEAN Initialized;
    WDFDEVICE ControlDevice;
    WDFDEVICE TpmDevice;
    WDFWAITLOCK Lock;
    LIST_ENTRY PendingList;
    ULONG PendingCount;
    PSOFTTPM_PENDING_COMMAND Current;
    BOOLEAN CurrentDelivered;
    BOOLEAN DeliveryInProgress;
    ULONG NextRequestId;
    WDFQUEUE CommandQueue;
    WDFTIMER TimeoutTimer;
    BOOLEAN ServiceConnected;
    WDFFILEOBJECT ServiceFile;
    ULONG CompletedCommandCount;
    ULONG TimeoutCount;
    ULONG UnknownIoctlCount;
    ULONG LastTpmResponseCode;
    SOFTTPM_TRACE_ENTRY Trace[SOFTTPM_TRACE_ENTRY_COUNT];
    ULONG TraceNext;
    ULONG TraceCount;
    ULONG TraceSequence;
} SOFTTPM_GLOBAL_STATE, *PSOFTTPM_GLOBAL_STATE;

PSOFTTPM_GLOBAL_STATE SoftTpmGetGlobalState(void);

VOID SoftTpmDebugRecord(PCSTR Name, ULONG Value);

VOID SoftTpmErrorRecord(ULONG Step, NTSTATUS Status);

EVT_WDF_DRIVER_DEVICE_ADD SoftTpmEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD SoftTpmEvtDriverUnload;

EVT_WDF_FILE_CLEANUP SoftTpmEvtControlFileCleanup;
EVT_WDF_DEVICE_FILE_CREATE SoftTpmEvtControlFileCreate;

#endif
