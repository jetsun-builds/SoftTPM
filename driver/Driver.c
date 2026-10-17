#include "Driver.h"
#include "ServiceLink.h"
#include "TpmControlDevice.h"
#include "TpmIoctl.h"

static SOFTTPM_GLOBAL_STATE g_SoftTpmState;
static WCHAR g_debugKeyBuffer[256];
static UNICODE_STRING g_debugKeyPath;
static ULONG g_debugAddDeviceCount;
static PDRIVER_OBJECT g_driverObject;

#define SOFTTPM_ERROR_CODE_BASE 0x1000

PSOFTTPM_GLOBAL_STATE SoftTpmGetGlobalState(void)
{
    return &g_SoftTpmState;
}

VOID SoftTpmDebugRecord(PCSTR Name, ULONG Value)
{
    WCHAR nameBuffer[64];
    UNICODE_STRING valueName;
    OBJECT_ATTRIBUTES attributes;
    HANDLE key = NULL;
    NTSTATUS status;
    ULONG i;

    if (g_debugKeyPath.Buffer == NULL)
    {
        return;
    }

    for (i = 0; Name[i] != 0 && i < 63; i++)
    {
        nameBuffer[i] = (WCHAR)(UCHAR)Name[i];
    }
    nameBuffer[i] = 0;

    valueName.Buffer = nameBuffer;
    valueName.Length = (USHORT)(i * sizeof(WCHAR));
    valueName.MaximumLength = valueName.Length;

    InitializeObjectAttributes(
        &attributes,
        &g_debugKeyPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    status = ZwOpenKey(&key, KEY_SET_VALUE, &attributes);
    if (NT_SUCCESS(status))
    {
        ZwSetValueKey(key, &valueName, 0, REG_DWORD, &Value, sizeof(Value));
        ZwClose(key);
    }
}

VOID SoftTpmErrorRecord(ULONG Step, NTSTATUS Status)
{
    PIO_ERROR_LOG_PACKET entry;
    ULONG size;

    if (g_driverObject == NULL)
    {
        return;
    }

    size = sizeof(IO_ERROR_LOG_PACKET) + sizeof(ULONG);
    entry = (PIO_ERROR_LOG_PACKET)IoAllocateErrorLogEntry(
        g_driverObject,
        (UCHAR)size);

    if (entry == NULL)
    {
        return;
    }

    entry->DumpDataSize = 2 * sizeof(ULONG);
    entry->ErrorCode = (NTSTATUS)(SOFTTPM_ERROR_CODE_BASE + Step);
    entry->UniqueErrorValue = Step;
    entry->FinalStatus = Status;
    entry->DumpData[0] = (ULONG)Status;
    entry->DumpData[1] = Step;

    IoWriteErrorLogEntry(entry);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    NTSTATUS status;
    USHORT copyLength;

    g_driverObject = DriverObject;

    copyLength = RegistryPath->Length;
    if (copyLength > (USHORT)(sizeof(g_debugKeyBuffer) - sizeof(WCHAR)))
    {
        copyLength = (USHORT)(sizeof(g_debugKeyBuffer) - sizeof(WCHAR));
    }
    RtlCopyMemory(g_debugKeyBuffer, RegistryPath->Buffer, copyLength);
    g_debugKeyBuffer[copyLength / sizeof(WCHAR)] = 0;
    g_debugKeyPath.Buffer = g_debugKeyBuffer;
    g_debugKeyPath.Length = copyLength;
    g_debugKeyPath.MaximumLength = (USHORT)sizeof(g_debugKeyBuffer);

    g_debugAddDeviceCount = 0;

    SoftTpmErrorRecord(1, STATUS_SUCCESS);
    SoftTpmDebugRecord("DbgEntry", 1);

    WDF_DRIVER_CONFIG_INIT(&config, SoftTpmEvtDeviceAdd);
    config.EvtDriverUnload = SoftTpmEvtDriverUnload;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        &driver);
    SoftTpmDebugRecord("DbgWdfDriver", (ULONG)status);
    SoftTpmErrorRecord(2, status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &state->Lock);
    SoftTpmDebugRecord("DbgLock", (ULONG)status);
    SoftTpmErrorRecord(3, status);
    if (!NT_SUCCESS(status))
    {
        state->Lock = NULL;
        return status;
    }

    InitializeListHead(&state->PendingList);

    status = SoftTpmCreateTpmDevice(driver, state);
    SoftTpmDebugRecord("DbgTpmDevice", (ULONG)status);
    SoftTpmErrorRecord(4, status);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(state->Lock);
        state->Lock = NULL;
        return status;
    }

    status = SoftTpmCreateControlDevice(driver, state);
    SoftTpmDebugRecord("DbgControlDevice", (ULONG)status);
    SoftTpmErrorRecord(5, status);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(state->TpmDevice);
        state->TpmDevice = NULL;
        WdfObjectDelete(state->Lock);
        state->Lock = NULL;
        return status;
    }

    state->Initialized = TRUE;

    SoftTpmErrorRecord(6, STATUS_SUCCESS);
    SoftTpmDebugRecord("DbgFinal", 0);

    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "SoftTPM: driver initialized\n");

    return STATUS_SUCCESS;
}

NTSTATUS SoftTpmEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    PSOFTTPM_GLOBAL_STATE state = SoftTpmGetGlobalState();
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    g_debugAddDeviceCount++;
    SoftTpmErrorRecord(7, STATUS_SUCCESS);
    SoftTpmDebugRecord("DbgAddDeviceCount", g_debugAddDeviceCount);

    WdfDeviceInitSetDeviceType(DeviceInit, SOFTTPM_DEVICE_TYPE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SOFTTPM_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    SoftTpmDebugRecord("DbgAddDeviceStatus", (ULONG)status);
    SoftTpmErrorRecord(8, status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    SoftTpmGetDeviceContext(device)->Device = device;

    if (!state->Initialized)
    {
        SoftTpmErrorRecord(9, STATUS_DEVICE_NOT_READY);
        SoftTpmDebugRecord("DbgAddDeviceStatus", (ULONG)STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }

    return STATUS_SUCCESS;
}

VOID SoftTpmEvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    SoftTpmErrorRecord(10, STATUS_SUCCESS);
    SoftTpmDebugRecord("DbgUnload", 1);

    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "SoftTPM: driver unloaded\n");
}
