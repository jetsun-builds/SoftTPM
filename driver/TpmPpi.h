#ifndef SOFTTPM_TPM_PPI_H
#define SOFTTPM_TPM_PPI_H

#include "Driver.h"

VOID SoftTpmTpmPpiCommand(PSOFTTPM_GLOBAL_STATE State, WDFREQUEST Request);

VOID SoftTpmPpiInitialize(void);

#endif
