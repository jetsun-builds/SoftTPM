#include "Driver.h"
#include "ServiceLink.h"
#include "TpmIoctl.h"
#include "TpmPpi.h"

#define SOFTTPM_TPM_COMMAND_HEADER_SIZE 10

static VOID SoftTpmTpmGetDeviceInfo(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PSOFTTPM_TPM_DEVICE_INFO info = NULL;
    PVOID buffer = NULL;
    size_t length = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(State);

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(SOFTTPM_TPM_DEVICE_INFO),
        &buffer,
        &length);

    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    info = (PSOFTTPM_TPM_DEVICE_INFO)buffer;
    info->StructVersion = 1;
    info->TpmVersion = SOFTTPM_TPM_VERSION_20;
    info->TpmInterfaceType = SOFTTPM_TPM_IFTYPE_HW;
    info->TpmImpRevision = 0;

    WdfRequestCompleteWithInformation(
        Request,
        STATUS_SUCCESS,
        sizeof(SOFTTPM_TPM_DEVICE_INFO));
}

static VOID SoftTpmTpmCompleteDword(WDFREQUEST Request, ULONG Value)
{
    PVOID buffer = NULL;
    size_t length = 0;

    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(ULONG),
            &buffer,
            &length)))
    {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    *(ULONG*)buffer = Value;

    WdfRequestCompleteWithInformation(
        Request,
        STATUS_SUCCESS,
        sizeof(ULONG));
}

static BOOLEAN SoftTpmTpmServiceConnected(PSOFTTPM_GLOBAL_STATE State)
{
    BOOLEAN connected;

    WdfWaitLockAcquire(State->Lock, NULL);
    connected = State->ServiceConnected;
    WdfWaitLockRelease(State->Lock);

    return connected;
}

static VOID SoftTpmTpmSubmitCommand(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;
    ULONG priority;
    NTSTATUS status;

    if (!SoftTpmTpmServiceConnected(State))
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        SOFTTPM_TPM_COMMAND_HEADER_SIZE,
        &inputBuffer,
        &inputLength);

    if (!NT_SUCCESS(status))
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (inputLength > SOFTTPM_MAX_PACKET_SIZE)
    {
        WdfRequestComplete(Request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }

    priority = ((PUCHAR)inputBuffer)[0];

    SoftTpmServiceLinkEnqueueCommand(
        State,
        Request,
        SOFTTPM_ACTION_SUBMIT,
        0,
        (PUCHAR)inputBuffer,
        (ULONG)inputLength,
        priority);
}

static VOID SoftTpmTpmSubmitCommandLocality(
    PSOFTTPM_GLOBAL_STATE State,
    WDFREQUEST Request)
{
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;
    size_t headerSize;
    ULONG locality;
    ULONG priority;
    PUCHAR command;
    size_t commandLength;
    NTSTATUS status;

    headerSize = FIELD_OFFSET(SOFTTPM_TPM_LOCALITY_INPUT, Command);

    if (!SoftTpmTpmServiceConnected(State))
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        headerSize,
        &inputBuffer,
        &inputLength);

    if (!NT_SUCCESS(status) || inputLength < headerSize)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    RtlCopyMemory(&locality, inputBuffer, sizeof(locality));
    RtlCopyMemory(&priority, (PUCHAR)inputBuffer + sizeof(ULONG), sizeof(priority));

    command = (PUCHAR)inputBuffer + headerSize;
    commandLength = inputLength - headerSize;

    if (locality > SOFTTPM_MAX_LOCALITY
        || commandLength < SOFTTPM_TPM_COMMAND_HEADER_SIZE
        || commandLength > SOFTTPM_MAX_PACKET_SIZE)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    SoftTpmServiceLinkEnqueueCommand(
        State,
        Request,
        SOFTTPM_ACTION_SUBMIT,
        locality,
        command,
        (ULONG)commandLength,
        priority);
}

static VOID SoftTpmTpmCreateWindowsKey(
    PSOFTTPM_GLOBAL_STATE State,
    WDFREQUEST Request)
{
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;
    ULONG keyHandle;
    NTSTATUS status;

    if (!SoftTpmTpmServiceConnected(State))
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(ULONG),
        &inputBuffer,
        &inputLength);

    if (!NT_SUCCESS(status) || inputLength < sizeof(ULONG))
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    RtlCopyMemory(&keyHandle, inputBuffer, sizeof(keyHandle));

    if (keyHandle != 0x81000001 && keyHandle != 0x81010001)
    {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    SoftTpmServiceLinkEnqueueCommand(
        State,
        Request,
        SOFTTPM_ACTION_CREATE_WINDOWS_KEY,
        0,
        NULL,
        0,
        0xC8);
}

static VOID SoftTpmTpmRevokeAttestation(
    PSOFTTPM_GLOBAL_STATE State,
    WDFREQUEST Request)
{
    if (!SoftTpmTpmServiceConnected(State))
    {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    SoftTpmServiceLinkEnqueueCommand(
        State,
        Request,
        SOFTTPM_ACTION_REVOKE_ATTESTATION,
        0,
        NULL,
        0,
        0xC8);
}

VOID SoftTpmEvtTpmIoControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    PVOID inputBuffer = NULL;
    size_t inputLength = 0;

    UNREFERENCED_PARAMETER(Queue);

    switch (IoControlCode)
    {
    case SOFTTPM_IOCTL_TPM_GET_DEVICE_INFO:
        SoftTpmTpmGetDeviceInfo(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_SUBMIT_COMMAND:
    case SOFTTPM_IOCTL_TPM_SUBMIT_COMMAND_NONBLOCKING:
        /* tpm.sys routes both codes to the same submit handler. */
        SoftTpmTpmSubmitCommand(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_SUBMIT_COMMAND_LOCALITY:
        SoftTpmTpmSubmitCommandLocality(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_CREATE_WINDOWS_KEY:
        SoftTpmTpmCreateWindowsKey(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_REVOKE_ATTESTATION:
        SoftTpmTpmRevokeAttestation(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_PPI_COMMAND:
        SoftTpmTpmPpiCommand(state, Request);
        return;

    case SOFTTPM_IOCTL_TPM_CANCEL_COMMANDS:
        /* The SoftTPM submit path is synchronous, so there is nothing to
           cancel; report success like tpm.sys does. */
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case SOFTTPM_IOCTL_TPM_TEST_MOR_BIT:
        /* Memory overwrite request bit: not armed. */
        SoftTpmTpmCompleteDword(Request, 0);
        return;

    case SOFTTPM_IOCTL_TPM_TEST_INTERRUPT_INFORMATION:
        /* No interrupt information is available for a virtual TPM. */
        SoftTpmTpmCompleteDword(Request, 0);
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
            "SoftTPM: unknown TPM IOCTL 0x%08lX\n",
            IoControlCode);

        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

VOID SoftTpmEvtTpmDeviceCleanup(WDFOBJECT Object)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();

    UNREFERENCED_PARAMETER(Object);

    SoftTpmServiceLinkReleasePending(state);
}

NTSTATUS SoftTpmCreateTpmDevice(WDFDRIVER Driver, PSOFTTPM_GLOBAL_STATE State)
{
    PWDFDEVICE_INIT deviceInit;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device = NULL;
    NTSTATUS status;

    DECLARE_CONST_UNICODE_STRING(deviceName, SOFTTPM_TPM_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(linkName, SOFTTPM_TPM_LINK_NAME);
    DECLARE_CONST_UNICODE_STRING(
        sddl,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)");

    deviceInit = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (deviceInit == NULL)
    {
        SoftTpmDebugRecord("DbgTpmDevInit", 1);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(deviceInit, SOFTTPM_DEVICE_TYPE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgTpmDevName", (ULONG)status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SOFTTPM_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = SoftTpmEvtTpmDeviceCleanup;

    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgTpmDevCreate", (ULONG)status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    SoftTpmGetDeviceContext(device)->Device = device;

    status = WdfDeviceCreateSymbolicLink(device, &linkName);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgTpmDevLink", (ULONG)status);
        WdfObjectDelete(device);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = SoftTpmEvtTpmIoControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        SoftTpmDebugRecord("DbgTpmDevQueue", (ULONG)status);
        WdfObjectDelete(device);
        return status;
    }

    WdfControlFinishInitializing(device);

    SoftTpmDebugRecord("DbgTpmDevice", 0);
    State->TpmDevice = device;
    return STATUS_SUCCESS;
}
