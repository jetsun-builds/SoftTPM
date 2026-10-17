
#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include "driver_client.h"
#include "engine.h"
#include "log.h"
#include "SoftTpmProtocol.h"

#define SOFTTPM_RECONNECT_DELAY_MS 1000

static HANDLE SoftTpmOpenControlDevice(void)
{
    return CreateFileW(
        SOFTTPM_CONTROL_WIN32_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);
}

static int SoftTpmGetCommand(
    HANDLE Device,
    HANDLE StopEvent,
    SOFTTPM_COMMAND* Command,
    DWORD* BytesReturned)
{
    OVERLAPPED overlapped;
    HANDLE handles[2];
    DWORD waitResult;
    BOOL waitOk;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (overlapped.hEvent == NULL)
    {
        return -1;
    }

    if (!DeviceIoControl(
            Device,
            IOCTL_SOFTTPM_GET_COMMAND,
            NULL,
            0,
            Command,
            sizeof(*Command),
            BytesReturned,
            &overlapped))
    {
        if (GetLastError() != ERROR_IO_PENDING)
        {
            CloseHandle(overlapped.hEvent);
            return -1;
        }
    }

    handles[0] = StopEvent;
    handles[1] = overlapped.hEvent;

    waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

    if (waitResult == WAIT_OBJECT_0)
    {
        CancelIoEx(Device, &overlapped);
        GetOverlappedResult(Device, &overlapped, BytesReturned, TRUE);
        CloseHandle(overlapped.hEvent);
        return 0;
    }

    if (waitResult != WAIT_OBJECT_0 + 1)
    {
        CloseHandle(overlapped.hEvent);
        return -1;
    }

    waitOk = GetOverlappedResult(Device, &overlapped, BytesReturned, FALSE);
    CloseHandle(overlapped.hEvent);

    return waitOk ? 1 : -1;
}

static void SoftTpmDumpDriverTrace(HANDLE Device)
{
    SOFTTPM_TRACE_ENTRY entries[16];
    DWORD bytesReturned = 0;
    DWORD count;
    DWORD start;
    DWORD index;

    if (!DeviceIoControl(
            Device,
            IOCTL_SOFTTPM_READ_TRACE,
            NULL,
            0,
            entries,
            sizeof(entries),
            &bytesReturned,
            NULL))
    {
        return;
    }

    count = bytesReturned / sizeof(SOFTTPM_TRACE_ENTRY);
    start = count > 8 ? count - 8 : 0;

    for (index = start; index < count; index++)
    {
        const SOFTTPM_TRACE_ENTRY* entry = &entries[index];

        log_warn(
            "driver trace seq=%lu ioctl=0x%08lX in=%lu out=%lu status=0x%08lX "
            "data=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
            entry->Sequence,
            entry->IoControlCode,
            entry->InputSize,
            entry->OutputSize,
            entry->Status,
            entry->Data[0], entry->Data[1], entry->Data[2], entry->Data[3],
            entry->Data[4], entry->Data[5], entry->Data[6], entry->Data[7],
            entry->Data[8], entry->Data[9], entry->Data[10], entry->Data[11]);
    }
}

static int SoftTpmSendResponse(
    HANDLE Device,
    const SOFTTPM_COMMAND* Command,
    ULONG Status,
    const unsigned char* Response,
    ULONG ResponseSize)
{
    SOFTTPM_RESPONSE* message;
    DWORD inputSize;
    DWORD bytesReturned;

    if (ResponseSize > SOFTTPM_MAX_PACKET_SIZE)
    {
        ResponseSize = SOFTTPM_MAX_PACKET_SIZE;
    }

    message = (SOFTTPM_RESPONSE*)malloc(sizeof(*message));
    if (message == NULL)
    {
        return -1;
    }

    memset(message, 0, sizeof(*message));
    message->ProtocolVersion = SOFTTPM_PROTOCOL_VERSION;
    message->RequestId = Command->RequestId;
    message->Status = Status;
    message->ResponseSize = ResponseSize;

    if (ResponseSize > 0 && Response != NULL)
    {
        memcpy(message->Response, Response, ResponseSize);
    }

    inputSize = (DWORD)(FIELD_OFFSET(SOFTTPM_RESPONSE, Response) + ResponseSize);

    if (!DeviceIoControl(
            Device,
            IOCTL_SOFTTPM_SEND_RESPONSE,
            message,
            inputSize,
            NULL,
            0,
            &bytesReturned,
            NULL))
    {
        DWORD error = GetLastError();
        log_warn("driver: SEND_RESPONSE failed with error %lu (request %lu, %lu bytes)",
                 error, (unsigned long)message->RequestId, (unsigned long)ResponseSize);
        SoftTpmDumpDriverTrace(Device);
        free(message);
        return -1;
    }

    free(message);
    return 0;
}

static uint32_t SoftTpmReadBigEndian32(const unsigned char* Data)
{
    return ((uint32_t)Data[0] << 24)
        | ((uint32_t)Data[1] << 16)
        | ((uint32_t)Data[2] << 8)
        | (uint32_t)Data[3];
}

static int SoftTpmAnswerCommand(HANDLE Device, const SOFTTPM_COMMAND* Command)
{
    unsigned char response[SOFTTPM_MAX_PACKET_SIZE];
    uint32_t responseSize = sizeof(response);
    uint32_t commandCode = 0;
    uint32_t responseCode = 0;
    int rc;

    if (Command->CommandSize >= 10)
    {
        commandCode = SoftTpmReadBigEndian32(&Command->Command[6]);
    }

    rc = engine_execute(Command->Command, Command->CommandSize, response, &responseSize);

    if (rc != 0)
    {
        if (Command->CommandSize >= 10)
        {
            log_warn("driver: engine failed command 0x%08X (size %u)",
                     commandCode,
                     Command->CommandSize);
        }
        else
        {
            log_warn("driver: engine rejected a %u byte command", Command->CommandSize);
        }

        return SoftTpmSendResponse(Device, Command, 1, NULL, 0);
    }

    if (responseSize >= 10)
    {
        responseCode = SoftTpmReadBigEndian32(&response[6]);
    }

    log_debug("tbs: cmd 0x%08X (%u bytes) -> 0x%08X (%u bytes)",
              commandCode, Command->CommandSize, responseCode, responseSize);

    return SoftTpmSendResponse(Device, Command, 0, response, responseSize);
}

static int SoftTpmServeDevice(HANDLE Device, HANDLE StopEvent)
{
    SOFTTPM_COMMAND* command = (SOFTTPM_COMMAND*)malloc(sizeof(SOFTTPM_COMMAND));

    if (command == NULL)
    {
        return -1;
    }

    for (;;)
    {
        DWORD bytesReturned = 0;
        int rc = SoftTpmGetCommand(Device, StopEvent, command, &bytesReturned);

        if (rc == 0)
        {
            free(command);
            return 0;
        }

        if (rc < 0)
        {
            free(command);
            return -1;
        }

        if (bytesReturned < FIELD_OFFSET(SOFTTPM_COMMAND, Command)
            || command->ProtocolVersion != SOFTTPM_PROTOCOL_VERSION
            || command->CommandSize > SOFTTPM_MAX_PACKET_SIZE)
        {
            log_warn("driver: invalid GET_COMMAND payload (%lu bytes)", bytesReturned);
            free(command);
            return -1;
        }

        if (SoftTpmAnswerCommand(Device, command) != 0)
        {
            free(command);
            return -1;
        }
    }
}

int driver_client_run(const SOFTTPM_OPTIONS* Options, HANDLE StopEvent)
{
    int failureCount = 0;
    int connected = 0;

    (void)Options;

    for (;;)
    {
        HANDLE device = SoftTpmOpenControlDevice();

        if (device == INVALID_HANDLE_VALUE)
        {
            failureCount++;

            if (failureCount == 1 || (failureCount % 30) == 0)
            {
                log_warn("driver: cannot open %ls (error %lu), retrying",
                         SOFTTPM_CONTROL_WIN32_NAME, GetLastError());
            }

            if (WaitForSingleObject(StopEvent, SOFTTPM_RECONNECT_DELAY_MS) == WAIT_OBJECT_0)
            {
                return 0;
            }

            continue;
        }

        failureCount = 0;

        if (!connected)
        {
            log_info("driver: connected to %ls", SOFTTPM_CONTROL_WIN32_NAME);
            connected = 1;
        }

        if (SoftTpmServeDevice(device, StopEvent) == 0)
        {
            CloseHandle(device);
            return 0;
        }

        CloseHandle(device);

        if (connected)
        {
            log_warn("driver: disconnected, waiting for the control device");
            connected = 0;
        }

        if (WaitForSingleObject(StopEvent, SOFTTPM_RECONNECT_DELAY_MS) == WAIT_OBJECT_0)
        {
            return 0;
        }
    }
}
