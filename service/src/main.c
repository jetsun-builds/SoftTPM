
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "options.h"
#include "service_control.h"
#include "driver_client.h"
#include "engine.h"
#include "log.h"
#include "provision/provision.h"

static HANDLE s_consoleStopEvent = NULL;

static BOOL WINAPI SoftTpmConsoleControlHandler(DWORD Control)
{
    if (Control == CTRL_C_EVENT || Control == CTRL_BREAK_EVENT
        || Control == CTRL_CLOSE_EVENT || Control == CTRL_SHUTDOWN_EVENT)
    {
        if (s_consoleStopEvent != NULL)
        {
            SetEvent(s_consoleStopEvent);
        }
        return TRUE;
    }

    return FALSE;
}

static void SoftTpmAttachConsole(void)
{
    FILE* stream;
    HANDLE console;

    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
    }

    console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (console != NULL && console != INVALID_HANDLE_VALUE)
    {
        stream = freopen("CONOUT$", "w", stdout);
        (void)stream;
    }

    console = GetStdHandle(STD_ERROR_HANDLE);
    if (console != NULL && console != INVALID_HANDLE_VALUE)
    {
        stream = freopen("CONOUT$", "w", stderr);
        (void)stream;
    }
}

static int SoftTpmRunConsole(const SOFTTPM_OPTIONS* Options)
{
    int result = 0;

    SoftTpmAttachConsole();

    s_consoleStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (s_consoleStopEvent == NULL)
    {
        wprintf(L"cannot create stop event (error %lu)\n", GetLastError());
        return 1;
    }

    SetConsoleCtrlHandler(SoftTpmConsoleControlHandler, TRUE);

    log_init(Options->LogFilePath, Options->LogLevel, 1);
    log_info("console: starting (state directory %ls)", Options->StateDirectory);

    if (engine_start(Options->StateDirectory) != 0)
    {
        log_error("console: TPM engine failed to start");
        result = 1;
        goto cleanup;
    }

    if (provision_run(Options->StateDirectory) != 0)
    {
        log_warn("console: provisioning incomplete, continuing with limited functionality");
    }

    driver_client_run(Options, s_consoleStopEvent);
    engine_stop();

cleanup:
    log_close();
    CloseHandle(s_consoleStopEvent);
    s_consoleStopEvent = NULL;

    return result;
}

int WINAPI wWinMain(
    HINSTANCE Instance,
    HINSTANCE PreviousInstance,
    LPWSTR CommandLine,
    int ShowCommand)
{
    SOFTTPM_OPTIONS options;
    wchar_t** argv;
    int argc;
    int parseResult;

    (void)Instance;
    (void)PreviousInstance;
    (void)CommandLine;
    (void)ShowCommand;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL)
    {
        return 1;
    }

    options_set_defaults(&options);
    parseResult = options_parse(&options, argc, argv);

    LocalFree(argv);

    if (parseResult != 0)
    {
        return parseResult < 0 ? 1 : 0;
    }

    CreateDirectoryW(options.StateDirectory, NULL);

    if (options.Console)
    {
        return SoftTpmRunConsole(&options);
    }

    if (service_run(&options) != 0)
    {
        return 1;
    }

    return 0;
}
