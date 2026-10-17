#ifndef SOFTTPM_OPTIONS_H
#define SOFTTPM_OPTIONS_H

#include "log.h"
#include "storage/state_store.h"

typedef struct _SOFTTPM_OPTIONS
{
    int Console;
    int PublishWbcl;
    SOFTTPM_LOG_LEVEL LogLevel;
    wchar_t StateDirectory[SOFTTPM_STATE_MAX_PATH];
    wchar_t LogFilePath[SOFTTPM_STATE_MAX_PATH];
    wchar_t PlatformLogPath[SOFTTPM_STATE_MAX_PATH];
} SOFTTPM_OPTIONS;

void options_set_defaults(SOFTTPM_OPTIONS* Options);
int options_parse(SOFTTPM_OPTIONS* Options, int argc, wchar_t** argv);

#endif
