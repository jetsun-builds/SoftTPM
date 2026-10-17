#include "Driver.h"
#include "ServiceLink.h"

VOID SoftTpmTraceAdd(
    PSOFTTPM_GLOBAL_STATE State,
    ULONG IoControlCode,
    const UCHAR* InputData,
    ULONG InputSize,
    ULONG OutputSize,
    ULONG Status)
{
    PSOFTTPM_TRACE_ENTRY entry;

    WdfWaitLockAcquire(State->Lock, NULL);

    entry = &State->Trace[State->TraceNext];
    State->TraceSequence++;

    entry->Sequence = State->TraceSequence;
    entry->IoControlCode = IoControlCode;
    entry->InputSize = InputSize;
    entry->OutputSize = OutputSize;
    entry->Status = Status;

    RtlZeroMemory(entry->Data, SOFTTPM_TRACE_DATA_SIZE);
    if (InputData != NULL && InputSize > 0)
    {
        ULONG copySize = InputSize < SOFTTPM_TRACE_DATA_SIZE
            ? InputSize
            : SOFTTPM_TRACE_DATA_SIZE;

        RtlCopyMemory(entry->Data, InputData, copySize);
    }

    State->TraceNext = (State->TraceNext + 1) % SOFTTPM_TRACE_ENTRY_COUNT;
    if (State->TraceCount < SOFTTPM_TRACE_ENTRY_COUNT)
    {
        State->TraceCount++;
    }

    WdfWaitLockRelease(State->Lock);
}

VOID SoftTpmServiceLinkFillStatus(PSOFTTPM_GLOBAL_STATE State, PSOFTTPM_STATUS Status)
{
    WdfWaitLockAcquire(State->Lock, NULL);

    Status->ProtocolVersion = SOFTTPM_PROTOCOL_VERSION;
    if (State->ServiceConnected)
    {
        Status->State = SOFTTPM_STATE_RUNNING;
    }
    else if (State->Initialized)
    {
        Status->State = SOFTTPM_STATE_STOPPED;
    }
    else
    {
        Status->State = SOFTTPM_STATE_UNKNOWN;
    }
    Status->PendingCommandCount = State->PendingCount + (State->Current != NULL ? 1 : 0);
    Status->CompletedCommandCount = State->CompletedCommandCount;
    Status->TimeoutCount = State->TimeoutCount;
    Status->UnknownIoctlCount = State->UnknownIoctlCount;
    Status->LastTpmResponseCode = State->LastTpmResponseCode;

    WdfWaitLockRelease(State->Lock);
}

ULONG SoftTpmServiceLinkReadTrace(
    PSOFTTPM_GLOBAL_STATE State,
    PSOFTTPM_TRACE_ENTRY Buffer,
    ULONG MaxEntries)
{
    ULONG copied = 0;
    ULONG start;
    ULONG index;

    WdfWaitLockAcquire(State->Lock, NULL);

    copied = State->TraceCount < MaxEntries ? State->TraceCount : MaxEntries;
    start = (State->TraceNext + SOFTTPM_TRACE_ENTRY_COUNT - State->TraceCount)
        % SOFTTPM_TRACE_ENTRY_COUNT;

    for (index = 0; index < copied; index++)
    {
        Buffer[index] = State->Trace[(start + index) % SOFTTPM_TRACE_ENTRY_COUNT];
    }

    WdfWaitLockRelease(State->Lock);

    return copied;
}
