#ifndef SOFTTPM_SERVICE_CONTROL_H
#define SOFTTPM_SERVICE_CONTROL_H

#include "options.h"

#define SOFTTPM_SERVICE_NAME L"SoftTpmService"
#define SOFTTPM_SERVICE_DISPLAY_NAME L"SoftTPM Software TPM 2.0"

int service_run(SOFTTPM_OPTIONS* Options);

#endif
