#include "Driver.h"
#include "ServiceLink.h"

VOID SoftTpmServiceLinkFlush(PSOFTTPM_GLOBAL_STATE State)
{
    LIST_ENTRY failed;
    PSOFTTPM_PENDING_COMMAND entry;
    PSOFTTPM_PENDING_COMMAND current = NULL;
    BOOLEAN stopTimer = FALSE;

    InitializeListHead(&failed);

    /*
     * Drain any pending GET_COMMAND requests left behind by the old
     * connection and reopen the queue for the next one.  Purge alone
     * leaves the queue stopped, which would reject every future forward.
     */
    WdfIoQueuePurgeSynchronously(State->CommandQueue);
    WdfIoQueueStart(State->CommandQueue);

    WdfWaitLockAcquire(State->Lock, NULL);

    State->ServiceConnected = FALSE;
    State->ServiceFile = NULL;

    if (State->Current != NULL && !State->DeliveryInProgress)
    {
        current = State->Current;
        current->Completed = TRUE;
        State->Current = NULL;
        State->CurrentDelivered = FALSE;
        stopTimer = TRUE;
    }

    while (!IsListEmpty(&State->PendingList))
    {
        entry = CONTAINING_RECORD(
            State->PendingList.Flink,
            SOFTTPM_PENDING_COMMAND,
            ListEntry);

        RemoveEntryList(&entry->ListEntry);
        InsertTailList(&failed, &entry->ListEntry);
    }

    State->PendingCount = 0;

    WdfWaitLockRelease(State->Lock);

    if (stopTimer)
    {
        WdfTimerStop(State->TimeoutTimer, TRUE);
    }

    if (current != NULL)
    {
        SoftTpmCompleteTbsFailure(current->TbsRequest);
        ExFreePoolWithTag(current, SOFTTPM_POOL_TAG);
    }

    while (!IsListEmpty(&failed))
    {
        entry = CONTAINING_RECORD(
            failed.Flink,
            SOFTTPM_PENDING_COMMAND,
            ListEntry);

        RemoveEntryList(&entry->ListEntry);
        SoftTpmCompleteTbsFailure(entry->TbsRequest);
        ExFreePoolWithTag(entry, SOFTTPM_POOL_TAG);
    }
}

VOID SoftTpmServiceLinkReleasePending(PSOFTTPM_GLOBAL_STATE State)
{
    LIST_ENTRY released;
    PSOFTTPM_PENDING_COMMAND entry;

    InitializeListHead(&released);

    WdfWaitLockAcquire(State->Lock, NULL);

    if (State->Current != NULL)
    {
        InsertTailList(&released, &State->Current->ListEntry);
        State->Current = NULL;
        State->CurrentDelivered = FALSE;
    }

    while (!IsListEmpty(&State->PendingList))
    {
        entry = CONTAINING_RECORD(
            State->PendingList.Flink,
            SOFTTPM_PENDING_COMMAND,
            ListEntry);

        RemoveEntryList(&entry->ListEntry);
        InsertTailList(&released, &entry->ListEntry);
    }

    State->PendingCount = 0;

    WdfWaitLockRelease(State->Lock);

    while (!IsListEmpty(&released))
    {
        entry = CONTAINING_RECORD(
            released.Flink,
            SOFTTPM_PENDING_COMMAND,
            ListEntry);

        RemoveEntryList(&entry->ListEntry);
        ExFreePoolWithTag(entry, SOFTTPM_POOL_TAG);
    }
}

VOID SoftTpmEvtTimeoutTimer(WDFTIMER Timer)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    PSOFTTPM_PENDING_COMMAND current = NULL;
    BOOLEAN refresh = FALSE;

    UNREFERENCED_PARAMETER(Timer);

    WdfWaitLockAcquire(state->Lock, NULL);

    if (state->Current == NULL || state->Current->Completed)
    {
    }
    else if (state->DeliveryInProgress)
    {
        refresh = TRUE;
    }
    else
    {
        current = state->Current;
        current->Completed = TRUE;
        state->Current = NULL;
        state->CurrentDelivered = FALSE;
        state->TimeoutCount++;
    }

    WdfWaitLockRelease(state->Lock);

    if (refresh)
    {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1000));
        return;
    }

    if (current == NULL)
    {
        return;
    }

    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_WARNING_LEVEL,
        "SoftTPM: command %lu timed out\n",
        current->RequestId);

    SoftTpmTraceAdd(
        state,
        0,
        current->Request,
        current->RequestLength,
        0,
        (ULONG)STATUS_IO_TIMEOUT);
    SoftTpmCompleteTbsFailure(current->TbsRequest);
    ExFreePoolWithTag(current, SOFTTPM_POOL_TAG);

    SoftTpmServiceLinkTryDeliver(state);
}
