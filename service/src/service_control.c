
#include <windows.h>
#include "service_control.h"
#include "driver_client.h"
#include "engine.h"
#include "log.h"
#include "platform/platform_wbcl.h"
#include "platform/platform_log.h"
#include "provision/provision.h"

static SOFTTPM_OPTIONS* s_options = NULL;
static SERVICE_STATUS_HANDLE s_statusHandle = NULL;
static SERVICE_STATUS s_status;
static HANDLE s_stopEvent = NULL;

static void SoftTpmReportStatus(DWORD State, DWORD ExitCode, DWORD WaitHint)
{
    static DWORD checkpoint = 1;

    s_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    s_status.dwCurrentState = State;

    if (State == SERVICE_START_PENDING)
    {
        s_status.dwControlsAccepted = 0;
    }
    else
    {
        s_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    s_status.dwWin32ExitCode = ExitCode;
    s_status.dwServiceSpecificExitCode =
        (ExitCode == ERROR_SERVICE_SPECIFIC_ERROR) ? 1 : 0;
    s_status.dwCheckPoint = 0;
    s_status.dwWaitHint = WaitHint;

    if (State == SERVICE_RUNNING || State == SERVICE_STOPPED)
    {
        s_status.dwCheckPoint = 0;
    }
    else
    {
        s_status.dwCheckPoint = checkpoint++;
    }

    if (s_statusHandle != NULL)
    {
        SetServiceStatus(s_statusHandle, &s_status);
    }
}

static DWORD WINAPI SoftTpmServiceControlHandler(
    DWORD Control,
    DWORD EventType,
    VOID* EventData,
    VOID* Context)
{
    (void)EventType;
    (void)EventData;
    (void)Context;

    switch (Control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SoftTpmReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        if (s_stopEvent != NULL)
        {
            SetEvent(s_stopEvent);
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static void WINAPI SoftTpmServiceMain(DWORD Argc, LPWSTR* Argv)
{
    (void)Argc;
    (void)Argv;

    s_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    s_statusHandle = RegisterServiceCtrlHandlerExW(
        SOFTTPM_SERVICE_NAME,
        SoftTpmServiceControlHandler,
        NULL);

    if (s_statusHandle == NULL || s_stopEvent == NULL)
    {
        return;
    }

    SoftTpmReportStatus(SERVICE_START_PENDING, NO_ERROR, 30000);

    log_init(s_options->LogFilePath, s_options->LogLevel, 0);
    log_info("service: starting (state directory %ls)", s_options->StateDirectory);

    if (engine_start(s_options->StateDirectory) != 0)
    {
        log_error("service: TPM engine failed to start");
        SoftTpmReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return;
    }

    platform_log_load_and_replay(s_options->PlatformLogPath);

    if (provision_run(s_options->StateDirectory) != 0)
    {
        log_warn("service: provisioning incomplete, continuing with limited functionality");
    }

    if (s_options->PublishWbcl)
    {
        platform_wbcl_publish();
    }

    SoftTpmReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    driver_client_run(s_options, s_stopEvent);

    engine_stop();

    SoftTpmReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    log_info("service: stopped");

    log_close();
    CloseHandle(s_stopEvent);
    s_stopEvent = NULL;

    SoftTpmReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

int service_run(SOFTTPM_OPTIONS* Options)
{
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        {(LPWSTR)SOFTTPM_SERVICE_NAME, SoftTpmServiceMain},
        {NULL, NULL},
    };

    s_options = Options;

    if (!StartServiceCtrlDispatcherW(dispatchTable))
    {
        return -1;
    }

    return 0;
}
