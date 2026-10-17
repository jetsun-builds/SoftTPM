#ifndef SOFTTPM_TPM_IOCTL_H
#define SOFTTPM_TPM_IOCTL_H

#include "Driver.h"

/* tbs.sys builds its IOCTLs with DeviceType FILE_DEVICE_UNKNOWN (0x22) and
   FILE_READ_DATA|FILE_WRITE_DATA access, not with the SoftTPM device type.
   This is the complete set TBS emits; each code is named after the tbs.dll
   API that sends it (recovered from tbs.dll and tpm.sys disassembly):
     function 1      Tbsip_Cancel_Commands               = 0x22C004
     function 3      Tbsip_Submit_Command                = 0x22C00C
     function 5      Tbsi_Physical_Presence_Command      = 0x22C014
     function 7      Tbsi_GetDeviceInfo                  = 0x22C01C
     function 0x65   Tbsip_Submit_Command_NonBlocking    = 0x22C194
     function 0x66   Tbsip_TestMorBit                    = 0x22C198
     function 0x68   Tbsip_TestInterruptInformation      = 0x22C1A0
   (function 0x67, Tbsi_Create_Windows_Key, is not implemented.) */
#define SOFTTPM_IOCTL_TPM_CANCEL_COMMANDS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_SUBMIT_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_PPI_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 5, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_GET_DEVICE_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 7, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_SUBMIT_COMMAND_NONBLOCKING \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x65, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_TEST_MOR_BIT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x66, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_IOCTL_TPM_TEST_INTERRUPT_INFORMATION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x68, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define SOFTTPM_TPM_VERSION_20 2
#define SOFTTPM_TPM_IFTYPE_HW 3

typedef struct _SOFTTPM_TPM_DEVICE_INFO
{
    ULONG StructVersion;
    ULONG TpmVersion;
    ULONG TpmInterfaceType;
    ULONG TpmImpRevision;
} SOFTTPM_TPM_DEVICE_INFO, *PSOFTTPM_TPM_DEVICE_INFO;

NTSTATUS SoftTpmCreateTpmDevice(WDFDRIVER Driver, PSOFTTPM_GLOBAL_STATE State);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SoftTpmEvtTpmIoControl;

EVT_WDF_OBJECT_CONTEXT_CLEANUP SoftTpmEvtTpmDeviceCleanup;

#endif
