#include "Driver.h"
#include "ServiceLink.h"

static const UCHAR g_FailureResponse[SOFTTPM_FAILURE_RESPONSE_SIZE] =
{
    0x80, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x01, 0x01
};

static ULONG SoftTpmReadBigEndian32(const UCHAR* Data)
{
    return ((ULONG)Data[0] << 24)
        | ((ULONG)Data[1] << 16)
        | ((ULONG)Data[2] << 8)
        | (ULONG)Data[3];
}

static NTSTATUS SoftTpmCompleteGetCommandRequest(
    WDFREQUEST Request,
    PSOFTTPM_PENDING_COMMAND Command)
{
    PSOFTTPM_COMMAND output = NULL;
    PVOID buffer = NULL;
    size_t length = 0;
    size_t required;
    NTSTATUS status;

    required = FIELD_OFFSET(SOFTTPM_COMMAND, Command) + Command->RequestLength;

    status = WdfRequestRetrieveOutputBuffer(Request, required, &buffer, &length);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return status;
    }

    output = (PSOFTTPM_COMMAND)buffer;
    output->ProtocolVersion = SOFTTPM_PROTOCOL_VERSION;
    output->RequestId = Command->RequestId;
    output->Action = Command->Action;
    output->Locality = Command->Locality;
    output->CommandSize = Command->RequestLength;

    if (Command->RequestLength > 0)
    {
        RtlCopyMemory(output->Command, Command->Request, Command->RequestLength);
    }

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, required);
    return STATUS_SUCCESS;
}

VOID SoftTpmCompleteTbsResponse(WDFREQUEST Request, PUCHAR Response, ULONG ResponseLength)
{
    PVOID buffer = NULL;
    size_t length = 0;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, ResponseLength, &buffer, &length);
    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    RtlCopyMemory(buffer, Response, ResponseLength);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, ResponseLength);
}

VOID SoftTpmCompleteTbsFailure(WDFREQUEST Request)
{
    SoftTpmCompleteTbsResponse(
        Request,
        (PUCHAR)g_FailureResponse,
        SOFTTPM_FAILURE_RESPONSE_SIZE);
}

VOID SoftTpmCompleteRequestFailure(
    WDFREQUEST Request,
    ULONG Action,
    NTSTATUS Status)
{
    if (Action == SOFTTPM_ACTION_SUBMIT)
    {
        SoftTpmCompleteTbsFailure(Request);
    }
    else
    {
        WdfRequestComplete(Request, Status);
    }
}

VOID SoftTpmServiceLinkEnqueueCommand(
    PSOFTTPM_GLOBAL_STATE State,
    WDFREQUEST TbsRequest,
    ULONG Action,
    ULONG Locality,
    PUCHAR Command,
    ULONG CommandLength,
    ULONG Priority)
{
    PSOFTTPM_PENDING_COMMAND entry;
    PSOFTTPM_PENDING_COMMAND scan;
    PLIST_ENTRY item;
    BOOLEAN inserted = FALSE;

    entry = (PSOFTTPM_PENDING_COMMAND)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(SOFTTPM_PENDING_COMMAND),
        SOFTTPM_POOL_TAG);

    if (entry == NULL)
    {
        SoftTpmCompleteRequestFailure(
            TbsRequest,
            Action,
            STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    RtlZeroMemory(entry, sizeof(*entry));
    entry->TbsRequest = TbsRequest;
    entry->Action = Action;
    entry->Locality = Locality;
    entry->Priority = Priority;
    entry->RequestLength = CommandLength;

    if (CommandLength > 0 && Command != NULL)
    {
        RtlCopyMemory(entry->Request, Command, CommandLength);
        entry->Request[0] = 0x80;
    }

    WdfWaitLockAcquire(State->Lock, NULL);

    entry->RequestId = ++State->NextRequestId;

    if (State->PendingCount >= SOFTTPM_MAX_PENDING_COMMANDS)
    {
        WdfWaitLockRelease(State->Lock);
        ExFreePoolWithTag(entry, SOFTTPM_POOL_TAG);
        SoftTpmCompleteRequestFailure(
            TbsRequest,
            Action,
            STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    for (item = State->PendingList.Flink; item != &State->PendingList; item = item->Flink)
    {
        scan = CONTAINING_RECORD(item, SOFTTPM_PENDING_COMMAND, ListEntry);
        if (scan->Priority < entry->Priority)
        {
            InsertTailList(&scan->ListEntry, &entry->ListEntry);
            inserted = TRUE;
            break;
        }
    }

    if (!inserted)
    {
        InsertTailList(&State->PendingList, &entry->ListEntry);
    }

    State->PendingCount++;

    WdfWaitLockRelease(State->Lock);

    SoftTpmServiceLinkTryDeliver(State);
}

VOID SoftTpmServiceLinkTryDeliver(PSOFTTPM_GLOBAL_STATE State)
{
    PSOFTTPM_PENDING_COMMAND entry = NULL;
    PSOFTTPM_PENDING_COMMAND deliver = NULL;
    WDFREQUEST request = NULL;
    NTSTATUS status;
    BOOLEAN canDeliver = FALSE;
    BOOLEAN startTimer = FALSE;

    WdfWaitLockAcquire(State->Lock, NULL);

    if (State->Current == NULL && !IsListEmpty(&State->PendingList))
    {
        entry = CONTAINING_RECORD(
            State->PendingList.Flink,
            SOFTTPM_PENDING_COMMAND,
            ListEntry);

        RemoveEntryList(&entry->ListEntry);
        State->PendingCount--;
        State->Current = entry;
        State->CurrentDelivered = FALSE;
        startTimer = TRUE;
    }

    if (State->Current != NULL
        && !State->CurrentDelivered
        && !State->DeliveryInProgress
        && !State->Current->Completed)
    {
        State->DeliveryInProgress = TRUE;
        canDeliver = TRUE;
        deliver = State->Current;
    }

    WdfWaitLockRelease(State->Lock);

    if (startTimer)
    {
        WdfTimerStart(
            State->TimeoutTimer,
            WDF_REL_TIMEOUT_IN_MS(SOFTTPM_COMMAND_TIMEOUT_MS));
    }

    if (!canDeliver)
    {
        return;
    }

    status = WdfIoQueueRetrieveNextRequest(State->CommandQueue, &request);

    if (NT_SUCCESS(status))
    {
        /*
         * Mark the command delivered before completing the service request.
         * The service answers within microseconds of the completion and the
         * accept path requires CurrentDelivered to be set; setting it after
         * the completion lets a fast response be rejected with
         * STATUS_INVALID_PARAMETER, which tears down the connection.
         */
        WdfWaitLockAcquire(State->Lock, NULL);

        if (State->Current == deliver
            && !deliver->Completed
            && State->ServiceConnected)
        {
            State->CurrentDelivered = TRUE;
        }
        else
        {
            deliver = NULL;
        }

        State->DeliveryInProgress = FALSE;

        WdfWaitLockRelease(State->Lock);

        if (deliver != NULL)
        {
            status = SoftTpmCompleteGetCommandRequest(request, deliver);

            if (!NT_SUCCESS(status))
            {
                WdfWaitLockAcquire(State->Lock, NULL);

                if (State->Current == deliver && !deliver->Completed)
                {
                    State->CurrentDelivered = FALSE;
                }

                WdfWaitLockRelease(State->Lock);
            }
        }
        else
        {
            /*
             * The command completed or the service vanished while the request
             * was being retrieved; hand the request back to the service.
             */
            WdfRequestComplete(request, STATUS_CANCELLED);
        }
    }
    else
    {
        WdfWaitLockAcquire(State->Lock, NULL);
        State->DeliveryInProgress = FALSE;
        WdfWaitLockRelease(State->Lock);
    }
}

VOID SoftTpmServiceLinkAcceptResponse(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PVOID buffer = NULL;
    size_t inputLength = 0;
    size_t headerSize;
    PSOFTTPM_RESPONSE response;
    PSOFTTPM_PENDING_COMMAND current = NULL;
    BOOLEAN stopTimer = FALSE;
    ULONG responseCode = 0;
    ULONG currentRequestId = 0;
    ULONG deliveredFlag = 0;
    ULONG completedFlag = 0;
    ULONG connectedFlag = 0;
    NTSTATUS status;

    headerSize = FIELD_OFFSET(SOFTTPM_RESPONSE, Response);

    status = WdfRequestRetrieveInputBuffer(Request, headerSize, &buffer, &inputLength);
    if (!NT_SUCCESS(status))
    {
        /*
         * Leave a breadcrumb in the trace ring; the service dumps it when
         * SEND_RESPONSE fails, which pins down the rejection reason.
         */
        SoftTpmTraceAdd(
            State,
            IOCTL_SOFTTPM_SEND_RESPONSE,
            NULL,
            0,
            0xFFFF,
            STATUS_INVALID_PARAMETER);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    response = (PSOFTTPM_RESPONSE)buffer;

    if (response->ProtocolVersion != SOFTTPM_PROTOCOL_VERSION
        || response->ResponseSize > SOFTTPM_MAX_PACKET_SIZE
        || inputLength < headerSize + response->ResponseSize)
    {
        SoftTpmTraceAdd(
            State,
            IOCTL_SOFTTPM_SEND_RESPONSE,
            (const UCHAR*)response,
            12,
            0xFFFE,
            STATUS_INVALID_PARAMETER);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    WdfWaitLockAcquire(State->Lock, NULL);

    if (State->Current != NULL)
    {
        currentRequestId = State->Current->RequestId;
        deliveredFlag = State->CurrentDelivered ? 1 : 0;
        completedFlag = State->Current->Completed ? 1 : 0;
    }

    connectedFlag = State->ServiceConnected ? 1 : 0;

    if (State->Current != NULL
        && !State->Current->Completed
        && State->CurrentDelivered
        && State->Current->RequestId == response->RequestId)
    {
        current = State->Current;
        current->Completed = TRUE;
        State->Current = NULL;
        State->CurrentDelivered = FALSE;
        stopTimer = TRUE;
    }

    WdfWaitLockRelease(State->Lock);

    if (current == NULL)
    {
        ULONG diag[3];

        diag[0] = 0x524A4354; /* 'RJCT' */
        diag[1] = currentRequestId;
        diag[2] = response->RequestId;

        SoftTpmTraceAdd(
            State,
            IOCTL_SOFTTPM_SEND_RESPONSE,
            (const UCHAR*)diag,
            sizeof(diag),
            deliveredFlag | (completedFlag << 8) | (connectedFlag << 16),
            STATUS_INVALID_PARAMETER);

        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (stopTimer)
    {
        WdfTimerStop(State->TimeoutTimer, TRUE);
    }

    if (current->Action == SOFTTPM_ACTION_SUBMIT)
    {
        if (response->Status == 0 && response->ResponseSize >= 10)
        {
            responseCode = SoftTpmReadBigEndian32(response->Response + 6);
            SoftTpmCompleteTbsResponse(
                current->TbsRequest,
                response->Response,
                response->ResponseSize);
        }
        else
        {
            responseCode = 0x00000101;
            SoftTpmCompleteTbsFailure(current->TbsRequest);
        }
    }
    else
    {
        if (response->Status == 0 && response->ResponseSize == 0)
        {
            WdfRequestCompleteWithInformation(
                current->TbsRequest,
                STATUS_SUCCESS,
                0);
        }
        else
        {
            responseCode = 0x00000101;
            WdfRequestComplete(current->TbsRequest, STATUS_UNSUCCESSFUL);
        }
    }

    WdfWaitLockAcquire(State->Lock, NULL);
    State->CompletedCommandCount++;
    State->LastTpmResponseCode = responseCode;
    WdfWaitLockRelease(State->Lock);

    ExFreePoolWithTag(current, SOFTTPM_POOL_TAG);

    WdfRequestComplete(Request, STATUS_SUCCESS);

    SoftTpmServiceLinkTryDeliver(State);
}

BOOLEAN SoftTpmServiceLinkClaimServiceConnection(
    PSOFTTPM_GLOBAL_STATE State,
    WDFFILEOBJECT FileObject)
{
    BOOLEAN claimed = FALSE;

    WdfWaitLockAcquire(State->Lock, NULL);

    if (State->ServiceFile == NULL || State->ServiceFile == FileObject)
    {
        State->ServiceFile = FileObject;
        State->ServiceConnected = TRUE;
        claimed = TRUE;
    }

    WdfWaitLockRelease(State->Lock);

    return claimed;
}

VOID SoftTpmServiceLinkServiceDisconnected(PSOFTTPM_GLOBAL_STATE State)
{
    SoftTpmServiceLinkFlush(State);
}
