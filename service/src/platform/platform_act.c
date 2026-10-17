#include "platform_internal.h"

#if ACT_SUPPORT

static void ActSignal(P_ACT_DATA actData, int on)
{
    if (actData == NULL)
    {
        return;
    }

    if (on && actData->signaled == TRUE)
    {
        return;
    }

    actData->signaled = (uint8_t)on;

    switch (actData->number)
    {
#if RH_ACT_0
    case 0:
        return;
#endif
#if RH_ACT_A
    case 0xA:
        return;
#endif
    default:
        return;
    }
}

static P_ACT_DATA ActGetDataPointer(uint32_t act)
{
#define RETURN_ACT_POINTER(N) \
    if (0x##N == act)         \
        return &ACT_##N;

    FOR_EACH_ACT(RETURN_ACT_POINTER)

    return (P_ACT_DATA)NULL;
}

LIB_EXPORT int _plat__ACT_GetImplemented(uint32_t act)
{
    return (ActGetDataPointer(act) != NULL);
}

LIB_EXPORT uint32_t _plat__ACT_GetRemaining(uint32_t act)
{
    P_ACT_DATA actData = ActGetDataPointer(act);
    uint32_t remain;

    if (actData == NULL)
    {
        return 0;
    }

    remain = actData->remaining;

    if (actData->pending)
    {
        remain = actData->newValue;
    }

    return remain;
}

LIB_EXPORT int _plat__ACT_GetSignaled(uint32_t act)
{
    P_ACT_DATA actData = ActGetDataPointer(act);

    if (actData == NULL)
    {
        return 0;
    }

    return (int)actData->signaled;
}

LIB_EXPORT void _plat__ACT_SetSignaled(uint32_t act, int on)
{
    ActSignal(ActGetDataPointer(act), on);
}

LIB_EXPORT int _plat__ACT_GetPending(uint32_t act)
{
    P_ACT_DATA actData = ActGetDataPointer(act);

    if (actData == NULL)
    {
        return 0;
    }

    return (int)actData->pending;
}

LIB_EXPORT int _plat__ACT_UpdateCounter(uint32_t act, uint32_t newValue)
{
    P_ACT_DATA actData = ActGetDataPointer(act);

    if (actData == NULL)
    {
        return TRUE;
    }

    if (actData->pending != 0)
    {
        return FALSE;
    }

    actData->newValue = newValue;
    actData->pending = TRUE;

    return TRUE;
}

LIB_EXPORT void _plat__ACT_EnableTicks(int enable)
{
    actTicksAllowed = enable;
}

static void ActDecrement(P_ACT_DATA actData)
{
    if (actData->pending)
    {
        if ((actData->newValue == 0) && (actData->remaining != 0))
        {
            actData->newValue = 1;
        }

        actData->remaining = actData->newValue;
        actData->pending = 0;
    }

    if ((actData->remaining != 0) && (actData->remaining != UINT32_MAX))
    {
        if ((actData->remaining -= 1) == 0)
        {
            ActSignal(actData, TRUE);
        }
    }

    if (actData->signaled && (actData->remaining > 0))
    {
        ActSignal(actData, FALSE);
    }
}

LIB_EXPORT void _plat__ACT_Tick(void)
{
    if (actTicksAllowed)
    {
#define DECREMENT_COUNT(N) ActDecrement(&ACT_##N);

        FOR_EACH_ACT(DECREMENT_COUNT)
    }
}

static void ActZero(uint32_t act, P_ACT_DATA actData)
{
    actData->remaining = 0;
    actData->newValue = 0;
    actData->pending = 0;
    actData->number = (uint8_t)act;
    ActSignal(actData, FALSE);
}

LIB_EXPORT int _plat__ACT_Initialize(void)
{
    actTicksAllowed = 0;

#define ZERO_ACT(N) ActZero(0x##N, &ACT_##N);

    FOR_EACH_ACT(ZERO_ACT)

    return TRUE;
}

#endif
