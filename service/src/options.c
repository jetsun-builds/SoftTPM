
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "options.h"

#define SOFTTPM_DEFAULT_DIR_NAME L"SoftTpm"
#define SOFTTPM_LOG_FILE_NAME L"softtpm.log"

static void SoftTpmCopyString(wchar_t* Destination, size_t DestinationCount, const wchar_t* Source)
{
    if (DestinationCount == 0)
    {
        return;
    }

    wcsncpy(Destination, Source, DestinationCount - 1);
    Destination[DestinationCount - 1] = L'\0';
}

static void SoftTpmJoinPath(wchar_t* Destination, size_t DestinationCount, const wchar_t* Directory, const wchar_t* Name)
{
    if (DestinationCount == 0)
    {
        return;
    }

    _snwprintf(Destination, DestinationCount, L"%ls\\%ls", Directory, Name);
    Destination[DestinationCount - 1] = L'\0';
}

static int SoftTpmParseLogLevel(const wchar_t* Value, SOFTTPM_LOG_LEVEL* Level)
{
    if (_wcsicmp(Value, L"debug") == 0)
    {
        *Level = SOFTTPM_LOG_DEBUG;
        return 0;
    }
    if (_wcsicmp(Value, L"info") == 0)
    {
        *Level = SOFTTPM_LOG_INFO;
        return 0;
    }
    if (_wcsicmp(Value, L"warn") == 0)
    {
        *Level = SOFTTPM_LOG_WARN;
        return 0;
    }
    if (_wcsicmp(Value, L"error") == 0)
    {
        *Level = SOFTTPM_LOG_ERROR;
        return 0;
    }
    if (_wcsicmp(Value, L"none") == 0)
    {
        *Level = SOFTTPM_LOG_NONE;
        return 0;
    }

    return -1;
}

void options_set_defaults(SOFTTPM_OPTIONS* Options)
{
    DWORD length;
    wchar_t programData[SOFTTPM_STATE_MAX_PATH];

    memset(Options, 0, sizeof(*Options));

    Options->Console = 0;
    Options->LogLevel = SOFTTPM_LOG_INFO;

    length = GetEnvironmentVariableW(L"ProgramData", programData, SOFTTPM_STATE_MAX_PATH);
    if (length == 0 || length >= SOFTTPM_STATE_MAX_PATH)
    {
        SoftTpmCopyString(programData, SOFTTPM_STATE_MAX_PATH, L"C:\\ProgramData");
    }

    SoftTpmJoinPath(
        Options->StateDirectory,
        SOFTTPM_STATE_MAX_PATH,
        programData,
        SOFTTPM_DEFAULT_DIR_NAME);

    SoftTpmJoinPath(
        Options->LogFilePath,
        SOFTTPM_STATE_MAX_PATH,
        Options->StateDirectory,
        SOFTTPM_LOG_FILE_NAME);
}

int options_parse(SOFTTPM_OPTIONS* Options, int argc, wchar_t** argv)
{
    int index;

    for (index = 1; index < argc; index++)
    {
        const wchar_t* arg = argv[index];

        if (_wcsicmp(arg, L"--console") == 0)
        {
            Options->Console = 1;
        }
        else if (_wcsicmp(arg, L"--help") == 0 || _wcsicmp(arg, L"-h") == 0 || _wcsicmp(arg, L"/?") == 0)
        {
            wprintf(L"SoftTpmService - software TPM 2.0 for Windows\n\n");
            wprintf(L"Usage: SoftTpmService.exe [options]\n\n");
            wprintf(L"  --console            run in the foreground instead of as a service\n");
            wprintf(L"  --log-level=LEVEL    debug, info, warn, error or none\n");
            wprintf(L"  --state-dir=PATH     directory holding TPM state (default: %%ProgramData%%\\SoftTpm)\n");
            wprintf(L"  --help               show this help\n");
            return 1;
        }
        else if (_wcsnicmp(arg, L"--log-level=", 12) == 0)
        {
            if (SoftTpmParseLogLevel(arg + 12, &Options->LogLevel) != 0)
            {
                wprintf(L"Invalid log level: %ls\n", arg + 12);
                return -1;
            }
        }
        else if (_wcsnicmp(arg, L"--state-dir=", 12) == 0)
        {
            if (arg[12] == L'\0')
            {
                wprintf(L"Empty state directory\n");
                return -1;
            }

            if ((size_t)wcslen(arg + 12) >= SOFTTPM_STATE_MAX_PATH)
            {
                wprintf(L"State directory path is too long\n");
                return -1;
            }

            SoftTpmCopyString(Options->StateDirectory, SOFTTPM_STATE_MAX_PATH, arg + 12);
            SoftTpmJoinPath(
                Options->LogFilePath,
                SOFTTPM_STATE_MAX_PATH,
                Options->StateDirectory,
                SOFTTPM_LOG_FILE_NAME);
        }
        else if (arg[0] == L'-')
        {
            wprintf(L"Unknown option: %ls\n", arg);
            return -1;
        }
    }

    return 0;
}
