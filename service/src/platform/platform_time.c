
#include <windows.h>
#include "platform_internal.h"

#define CLOCK_NOMINAL 30000
#define CLOCK_ADJUST_COARSE 300
#define CLOCK_ADJUST_MEDIUM 30
#define CLOCK_ADJUST_FINE 1
#define CLOCK_ADJUST_LIMIT 5000

LIB_EXPORT void _plat__TimerReset(void)
{
    s_lastSystemTime = 0;
    s_tpmTime = 0;
    s_adjustRate = CLOCK_NOMINAL;
    s_timerReset = TRUE;
    s_timerStopped = TRUE;
}

LIB_EXPORT void _plat__TimerRestart(void)
{
    s_timerStopped = TRUE;
}

LIB_EXPORT uint64_t _plat__RealTime(void)
{
    FILETIME fileTime;
    ULARGE_INTEGER ticks;

    GetSystemTimeAsFileTime(&fileTime);

    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;

    return ticks.QuadPart / 10000;
}

LIB_EXPORT uint64_t _plat__TimerRead(void)
{
    uint64_t timeDiff;
    uint64_t adjustedTimeDiff;
    uint64_t timeNow;
    uint64_t readjustedTimeDiff;

    timeNow = _plat__RealTime();

    if (s_lastSystemTime == 0)
    {
        s_lastSystemTime = timeNow;
        s_lastReportedTime = 0;
        s_realTimePrevious = 0;
    }

    if (timeNow < s_lastReportedTime)
    {
        s_lastSystemTime = timeNow;
    }

    s_lastReportedTime = s_lastReportedTime + timeNow - s_lastSystemTime;
    s_lastSystemTime = timeNow;
    timeNow = s_lastReportedTime;

    if (s_realTimePrevious >= timeNow)
    {
        return s_tpmTime;
    }

    timeDiff = timeNow - s_realTimePrevious;

    adjustedTimeDiff = (timeDiff * CLOCK_NOMINAL) / ((uint64_t)s_adjustRate);

    s_tpmTime += adjustedTimeDiff;

    readjustedTimeDiff = (adjustedTimeDiff * (uint64_t)s_adjustRate) / CLOCK_NOMINAL;

    s_realTimePrevious = s_realTimePrevious + readjustedTimeDiff;

    return s_tpmTime;
}

LIB_EXPORT int _plat__TimerWasReset(void)
{
    int retVal = s_timerReset;
    s_timerReset = FALSE;
    return retVal;
}

LIB_EXPORT int _plat__TimerWasStopped(void)
{
    int retVal = s_timerStopped;
    s_timerStopped = FALSE;
    return retVal;
}

LIB_EXPORT void _plat__ClockRateAdjust(_plat__ClockAdjustStep adjust)
{
    switch (adjust)
    {
    case PLAT_TPM_CLOCK_ADJUST_COARSE_SLOWER:
        s_adjustRate += CLOCK_ADJUST_COARSE;
        break;
    case PLAT_TPM_CLOCK_ADJUST_MEDIUM_SLOWER:
        s_adjustRate += CLOCK_ADJUST_MEDIUM;
        break;
    case PLAT_TPM_CLOCK_ADJUST_FINE_SLOWER:
        s_adjustRate += CLOCK_ADJUST_FINE;
        break;
    case PLAT_TPM_CLOCK_ADJUST_FINE_FASTER:
        s_adjustRate -= CLOCK_ADJUST_FINE;
        break;
    case PLAT_TPM_CLOCK_ADJUST_MEDIUM_FASTER:
        s_adjustRate -= CLOCK_ADJUST_MEDIUM;
        break;
    case PLAT_TPM_CLOCK_ADJUST_COARSE_FASTER:
        s_adjustRate -= CLOCK_ADJUST_COARSE;
        break;
    default:
        break;
    }

    if (s_adjustRate > (CLOCK_NOMINAL + CLOCK_ADJUST_LIMIT))
    {
        s_adjustRate = CLOCK_NOMINAL + CLOCK_ADJUST_LIMIT;
    }
    if (s_adjustRate < (CLOCK_NOMINAL - CLOCK_ADJUST_LIMIT))
    {
        s_adjustRate = CLOCK_NOMINAL - CLOCK_ADJUST_LIMIT;
    }
}
