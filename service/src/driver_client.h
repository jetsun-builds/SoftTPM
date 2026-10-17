#ifndef SOFTTPM_DRIVER_CLIENT_H
#define SOFTTPM_DRIVER_CLIENT_H

#include <windows.h>
#include "options.h"

int driver_client_run(const SOFTTPM_OPTIONS* Options, HANDLE StopEvent);

#endif
