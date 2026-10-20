#include "Driver.h"
#include "ServiceLink.h"
#include "TpmIoctl.h"
#include "TpmPpi.h"

/* Synthesizes the Physical Presence Interface for Tbsi_Physical_Presence_Command.
   The input carries the PPI function index in the first DWORD followed by the
   operation and its argument ("Arg2"/"Arg3"/"Arg4" of the TCG Physical Presence
   Interface Specification). Real tpm.sys forwards these to the firmware's ACPI
   method or to a vendor TPM command and returns whatever the platform reports;
   the replies below mirror the return values of TPM 2.0 platform firmware
   (coreboot src/drivers/tpm/ppi.c is the reference). */

#define SOFTTPM_PPI_MAX_RESPONSE_DWORDS 3

#define SOFTTPM_PPI_VERSION_STRING "1.3"

#define SOFTTPM_PPI_FUNCTION_GET_VERSION 1
#define SOFTTPM_PPI_FUNCTION_SUBMIT_OPERATION 2
#define SOFTTPM_PPI_FUNCTION_GET_PENDING_OPERATION 3
#define SOFTTPM_PPI_FUNCTION_GET_TRANSITION 4
#define SOFTTPM_PPI_FUNCTION_GET_OPERATION_RESPONSE 5
#define SOFTTPM_PPI_FUNCTION_SUBMIT_OPERATION_2 7
#define SOFTTPM_PPI_FUNCTION_GET_CONFIRMATION_STATUS 8

/* Return values from the Physical Presence Interface Specification. */
#define SOFTTPM_PPI_RET_SUCCESS 0
#define SOFTTPM_PPI_RET_REBOOT 2

/* Function 8: the operation is allowed; firmware asks the user for physical
   presence confirmation before applying it at the next boot. */
#define SOFTTPM_PPI_RET_ALLOWED_WITH_PP 3

/* Operations submitted through function 2/7, reported back through function 3.
   There is no pre-OS component that could apply them, so the pending request
   is persisted in the driver service key and restored after a reload. */
static ULONG SoftTpmPpiPendingOperation;
static ULONG SoftTpmPpiPendingArgument;

#define SOFTTPM_PPI_PENDING_OPERATION_VALUE L"PpiPendingOperation"
#define SOFTTPM_PPI_PENDING_ARGUMENT_VALUE L"PpiPendingArgument"

VOID SoftTpmPpiInitialize(void)
{
    ULONG value;

    if (NT_SUCCESS(SoftTpmRegistryReadDword(
            SOFTTPM_PPI_PENDING_OPERATION_VALUE, &value)))
    {
        SoftTpmPpiPendingOperation = value;
    }

    if (NT_SUCCESS(SoftTpmRegistryReadDword(
            SOFTTPM_PPI_PENDING_ARGUMENT_VALUE, &value)))
    {
        SoftTpmPpiPendingArgument = value;
    }
}

static VOID SoftTpmPpiPersist(void)
{
    SoftTpmRegistryWriteDword(
        SOFTTPM_PPI_PENDING_OPERATION_VALUE,
        SoftTpmPpiPendingOperation);
    SoftTpmRegistryWriteDword(
        SOFTTPM_PPI_PENDING_ARGUMENT_VALUE,
        SoftTpmPpiPendingArgument);
}

static NTSTATUS SoftTpmPpiBuildResponse(
    const ULONG* Input,
    size_t InputLength,
    ULONG Payload[SOFTTPM_PPI_MAX_RESPONSE_DWORDS],
    size_t* PayloadSize,
    size_t* RequiredOutputSize)
{
    ULONG function = Input[0];

    switch (function)
    {
    case SOFTTPM_PPI_FUNCTION_GET_VERSION:
        RtlCopyMemory(Payload, SOFTTPM_PPI_VERSION_STRING, sizeof(SOFTTPM_PPI_VERSION_STRING));
        *PayloadSize = sizeof(SOFTTPM_PPI_VERSION_STRING);
        *RequiredOutputSize = sizeof(SOFTTPM_PPI_VERSION_STRING);
        return STATUS_SUCCESS;

    case SOFTTPM_PPI_FUNCTION_SUBMIT_OPERATION:
    case SOFTTPM_PPI_FUNCTION_SUBMIT_OPERATION_2:
        if (InputLength < 2 * sizeof(ULONG))
        {
            return STATUS_INVALID_PARAMETER;
        }

        SoftTpmPpiPendingOperation = Input[1];
        SoftTpmPpiPendingArgument = 0;
        if (function == SOFTTPM_PPI_FUNCTION_SUBMIT_OPERATION_2 &&
            InputLength >= 3 * sizeof(ULONG))
        {
            SoftTpmPpiPendingArgument = Input[2];
        }

        SoftTpmPpiPersist();

        Payload[0] = SOFTTPM_PPI_RET_SUCCESS;
        *PayloadSize = sizeof(ULONG);
        *RequiredOutputSize = sizeof(ULONG);
        return STATUS_SUCCESS;

    case SOFTTPM_PPI_FUNCTION_GET_PENDING_OPERATION:
        Payload[0] = SOFTTPM_PPI_RET_SUCCESS;
        Payload[1] = SoftTpmPpiPendingOperation;
        Payload[2] = SoftTpmPpiPendingArgument;
        *PayloadSize = 3 * sizeof(ULONG);
        *RequiredOutputSize = 2 * sizeof(ULONG);
        return STATUS_SUCCESS;

    case SOFTTPM_PPI_FUNCTION_GET_TRANSITION:
        /* The pre-OS environment is entered by rebooting. */
        Payload[0] = SOFTTPM_PPI_RET_REBOOT;
        *PayloadSize = sizeof(ULONG);
        *RequiredOutputSize = sizeof(ULONG);
        return STATUS_SUCCESS;

    case SOFTTPM_PPI_FUNCTION_GET_OPERATION_RESPONSE:
        /* No pre-OS environment has applied an operation. */
        Payload[0] = SOFTTPM_PPI_RET_SUCCESS;
        Payload[1] = 0;
        Payload[2] = 0;
        *PayloadSize = 3 * sizeof(ULONG);
        *RequiredOutputSize = 3 * sizeof(ULONG);
        return STATUS_SUCCESS;

    case SOFTTPM_PPI_FUNCTION_GET_CONFIRMATION_STATUS:
        Payload[0] = SOFTTPM_PPI_RET_ALLOWED_WITH_PP;
        *PayloadSize = sizeof(ULONG);
        *RequiredOutputSize = sizeof(ULONG);
        return STATUS_SUCCESS;

    default:
        /* Function 0 is the ACPI query and function 6 the deprecated language
           selection; tpm.sys rejects both before they reach a driver. */
        return STATUS_INVALID_PARAMETER;
    }
}

VOID SoftTpmTpmPpiCommand(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request)
{
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t inputLength = 0;
    size_t outputLength = 0;
    ULONG payload[SOFTTPM_PPI_MAX_RESPONSE_DWORDS];
    size_t payloadSize = 0;
    size_t requiredOutputSize = 0;
    size_t copySize;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(ULONG),
        &inputBuffer,
        &inputLength);

    if (NT_SUCCESS(status))
    {
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            0,
            &outputBuffer,
            &outputLength);
    }

    if (NT_SUCCESS(status))
    {
        status = SoftTpmPpiBuildResponse(
            (const ULONG*)inputBuffer,
            inputLength,
            payload,
            &payloadSize,
            &requiredOutputSize);
    }

    if (NT_SUCCESS(status) && outputLength < requiredOutputSize)
    {
        status = STATUS_BUFFER_TOO_SMALL;
    }

    if (inputBuffer != NULL)
    {
        SoftTpmTraceAdd(
            State,
            SOFTTPM_IOCTL_TPM_PPI_COMMAND,
            (const UCHAR*)inputBuffer,
            (ULONG)inputLength,
            (ULONG)outputLength,
            (ULONG)status);
    }

    if (NT_SUCCESS(status))
    {
        copySize = payloadSize < outputLength ? payloadSize : outputLength;
        RtlCopyMemory(outputBuffer, payload, copySize);
        WdfRequestCompleteWithInformation(
            Request,
            STATUS_SUCCESS,
            (ULONG_PTR)copySize);
    }
    else
    {
        WdfRequestComplete(Request, status);
    }
}
