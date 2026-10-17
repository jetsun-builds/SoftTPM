
#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <stdarg.h>

#include "log.h"

static FILE* g_logFile = NULL;
static SOFTTPM_LOG_LEVEL g_logLevel = SOFTTPM_LOG_INFO;
static int g_logConsole = 0;
static CRITICAL_SECTION g_logLock;
static int g_logInitialized = 0;

static const char* LogLevelName(SOFTTPM_LOG_LEVEL level)
{
    switch (level)
    {
    case SOFTTPM_LOG_DEBUG:
        return "DEBUG";
    case SOFTTPM_LOG_INFO:
        return "INFO";
    case SOFTTPM_LOG_WARN:
        return "WARN";
    case SOFTTPM_LOG_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

void log_init(const wchar_t* FilePath, SOFTTPM_LOG_LEVEL Level, int AlsoConsole)
{
    if (!g_logInitialized)
    {
        InitializeCriticalSection(&g_logLock);
        g_logInitialized = 1;
    }

    EnterCriticalSection(&g_logLock);

    g_logLevel = Level;
    g_logConsole = AlsoConsole;

    if (FilePath != NULL)
    {
        FILE* file = _wfsopen(FilePath, L"a", _SH_DENYNO);
        if (file != NULL)
        {
            g_logFile = file;
        }
    }

    LeaveCriticalSection(&g_logLock);
}

void log_close(void)
{
    if (!g_logInitialized)
    {
        return;
    }

    EnterCriticalSection(&g_logLock);

    if (g_logFile != NULL)
    {
        fclose(g_logFile);
        g_logFile = NULL;
    }

    LeaveCriticalSection(&g_logLock);
    DeleteCriticalSection(&g_logLock);
    g_logInitialized = 0;
}

void log_set_level(SOFTTPM_LOG_LEVEL Level)
{
    EnterCriticalSection(&g_logLock);
    g_logLevel = Level;
    LeaveCriticalSection(&g_logLock);
}

static void LogWriteV(SOFTTPM_LOG_LEVEL Level, const char* Format, va_list Args)
{
    SYSTEMTIME time;
    char message[1024];
    char line[1200];

    if (!g_logInitialized || Level < g_logLevel || Level >= SOFTTPM_LOG_NONE)
    {
        return;
    }

    GetLocalTime(&time);
    vsnprintf(message, sizeof(message), Format, Args);

    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %-5s %s\n",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        LogLevelName(Level),
        message);

    EnterCriticalSection(&g_logLock);

    if (g_logFile != NULL)
    {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }

    if (g_logConsole)
    {
        fputs(line, stdout);
        fflush(stdout);
    }

    LeaveCriticalSection(&g_logLock);

    OutputDebugStringA(line);
}

void log_debug(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    LogWriteV(SOFTTPM_LOG_DEBUG, Format, args);
    va_end(args);
}

void log_info(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    LogWriteV(SOFTTPM_LOG_INFO, Format, args);
    va_end(args);
}

void log_warn(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    LogWriteV(SOFTTPM_LOG_WARN, Format, args);
    va_end(args);
}

void log_error(const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    LogWriteV(SOFTTPM_LOG_ERROR, Format, args);
    va_end(args);
}
