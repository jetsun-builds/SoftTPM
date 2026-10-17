#ifndef SOFTTPM_TPM_CONTROL_DEVICE_H
#define SOFTTPM_TPM_CONTROL_DEVICE_H

#include "Driver.h"

NTSTATUS SoftTpmCreateControlDevice(WDFDRIVER Driver, PSOFTTPM_GLOBAL_STATE State);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SoftTpmEvtControlIoControl;

#endif
