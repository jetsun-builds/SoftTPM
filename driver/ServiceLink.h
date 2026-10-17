#ifndef SOFTTPM_SERVICE_LINK_H
#define SOFTTPM_SERVICE_LINK_H

#include "Driver.h"

VOID SoftTpmServiceLinkEnqueueCommand(
    PSOFTTPM_GLOBAL_STATE State,
    WDFREQUEST TbsRequest,
    PUCHAR Command,
    ULONG CommandLength,
    ULONG Priority);

VOID SoftTpmServiceLinkTryDeliver(PSOFTTPM_GLOBAL_STATE State);

VOID SoftTpmServiceLinkAcceptResponse(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request);

BOOLEAN SoftTpmServiceLinkClaimServiceConnection(
    PSOFTTPM_GLOBAL_STATE State,
    WDFFILEOBJECT FileObject);

VOID SoftTpmServiceLinkServiceDisconnected(PSOFTTPM_GLOBAL_STATE State);

VOID SoftTpmServiceLinkFlush(PSOFTTPM_GLOBAL_STATE State);

VOID SoftTpmServiceLinkReleasePending(PSOFTTPM_GLOBAL_STATE State);

VOID SoftTpmServiceLinkFillStatus(PSOFTTPM_GLOBAL_STATE State, PSOFTTPM_STATUS Status);

ULONG SoftTpmServiceLinkReadTrace(
    PSOFTTPM_GLOBAL_STATE State,
    PSOFTTPM_TRACE_ENTRY Buffer,
    ULONG MaxEntries);

VOID SoftTpmTraceAdd(
    PSOFTTPM_GLOBAL_STATE State,
    ULONG IoControlCode,
    const UCHAR* InputData,
    ULONG InputSize,
    ULONG OutputSize,
    ULONG Status);

VOID SoftTpmCompleteTbsResponse(WDFREQUEST Request, PUCHAR Response, ULONG ResponseLength);

VOID SoftTpmCompleteTbsFailure(WDFREQUEST Request);

EVT_WDF_TIMER SoftTpmEvtTimeoutTimer;

#endif
