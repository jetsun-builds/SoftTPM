#include "Driver.h"
#include "ServiceLink.h"
#include "TpmControlDevice.h"

static VOID SoftTpmControlGetStatus(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PSOFTTPM_STATUS status = NULL;
    PVOID buffer = NULL;
    size_t length = 0;
    NTSTATUS ntStatus;

    ntStatus = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(SOFTTPM_STATUS),
        &buffer,
        &length);

    if (!NT_SUCCESS(ntStatus))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = (PSOFTTPM_STATUS)buffer;
    SoftTpmServiceLinkFillStatus(State, status);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(SOFTTPM_STATUS));
}

static VOID SoftTpmControlReadTrace(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PVOID buffer = NULL;
    size_t length = 0;
    ULONG maxEntries;
    ULONG copied;
    NTSTATUS ntStatus;

    ntStatus = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(SOFTTPM_TRACE_ENTRY),
        &buffer,
        &length);

    if (!NT_SUCCESS(ntStatus))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    maxEntries = (ULONG)(length / sizeof(SOFTTPM_TRACE_ENTRY));
    copied = SoftTpmServiceLinkReadTrace(State, (PSOFTTPM_TRACE_ENTRY)buffer, maxEntries);

    WdfRequestCompleteWithInformation(
        Request,
        STATUS_SUCCESS,
        copied * sizeof(SOFTTPM_TRACE_ENTRY));
}

VOID SoftTpmEvtControlIoControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);

    switch (IoControlCode)
    {
    case IOCTL_SOFTTPM_GET_COMMAND:
        /*
         * The first file object that polls for commands is the service.
         * Diagnostics clients only use GET_STATUS/READ_TRACE, so their
         * open/close cycles must never end the service connection.
         */
        if (!SoftTpmServiceLinkClaimServiceConnection(
                state,
                WdfRequestGetFileObject(Request)))
        {
            WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
            return;
        }

        status = WdfRequestForwardToIoQueue(Request, state->CommandQueue);
        if (!NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, status);
            return;
        }
        SoftTpmServiceLinkTryDeliver(state);
        return;

    case IOCTL_SOFTTPM_SEND_RESPONSE:
        SoftTpmServiceLinkAcceptResponse(state, Request);
        return;

    case IOCTL_SOFTTPM_GET_STATUS:
        SoftTpmControlGetStatus(state, Request);
        return;

    case IOCTL_SOFTTPM_READ_TRACE:
        SoftTpmControlReadTrace(state, Request);
        return;

    default:
        if (InputBufferLength > 0)
        {
            WdfRequestRetrieveInputBuffer(
                Request,
                0,
                &inputBuffer,
                &inputLength);
        }

        SoftTpmTraceAdd(
            state,
            IoControlCode,
            (const UCHAR*)inputBuffer,
            (ULONG)InputBufferLength,
            (ULONG)OutputBufferLength,
            (ULONG)STATUS_INVALID_DEVICE_REQUEST);

        WdfWaitLockAcquire(state->Lock, NULL);
        state->UnknownIoctlCount++;
        WdfWaitLockRelease(state->Lock);

        DbgPrintEx(
            DPFLTR_IHVDRIVER_ID,
            DPFLTR_WARNING_LEVEL,
            "SoftTPM: unknown control IOCTL 0x%08lX\n",
            IoControlCode);

        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

VOID SoftTpmEvtControlFileCreate(
    WDFDEVICE Device,
    WDFREQUEST Request,
    WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(FileObject);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID SoftTpmEvtControlFileCleanup(WDFFILEOBJECT FileObject)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    BOOLEAN isService;

    WdfWaitLockAcquire(state->Lock, NULL);
    isService = (state->ServiceFile != NULL && state->ServiceFile == FileObject);
    WdfWaitLockRelease(state->Lock);

    if (!isService)
    {
        return;
    }

    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "SoftTPM: service disconnected\n");

    SoftTpmServiceLinkServiceDisconnected(state);
}

VOID SoftTpmEvtControlDeviceCleanup(WDFOBJECT Object)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();

    UNREFERENCED_PARAMETER(Object);

    SoftTpmServiceLinkReleasePending(state);
}

NTSTATUS SoftTpmCreateControlDevice(WDFDRIVER Driver, PSOFTTPM_GLOBAL_STATE State)
{
    PWDFDEVICE_INIT deviceInit;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDFDEVICE device = NULL;
    NTSTATUS status;

    DECLARE_CONST_UNICODE_STRING(deviceName, SOFTTPM_CONTROL_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(linkName, SOFTTPM_CONTROL_LINK_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (deviceInit == NULL)
    {
        SoftTpmDebugRecord("DbgCtlDevInit", 1);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(deviceInit, SOFTTPM_DEVICE_TYPE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevName", (ULONG)status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        SoftTpmEvtControlFileCreate,
        NULL,
        SoftTpmEvtControlFileCleanup);

    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SOFTTPM_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = SoftTpmEvtControlDeviceCleanup;

    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevCreate", (ULONG)status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    SoftTpmGetDeviceContext(device)->Device = device;

    status = WdfDeviceCreateSymbolicLink(device, &linkName);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevLink", (ULONG)status);
        WdfObjectDelete(device);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = SoftTpmEvtControlIoControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevQueue", (ULONG)status);
        WdfObjectDelete(device);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &State->CommandQueue);

    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevCmdQueue", (ULONG)status);
        WdfObjectDelete(device);
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, SoftTpmEvtTimeoutTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;
    timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfTimerCreate(&timerConfig, &timerAttributes, &State->TimeoutTimer);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgCtlDevTimer", (ULONG)status);
        State->CommandQueue = NULL;
        WdfObjectDelete(device);
        return status;
    }

    WdfControlFinishInitializing(device);

    SoftTpmDebugRecord("DbgControlDevice", 0);
    State->ControlDevice = device;
    return STATUS_SUCCESS;
}
