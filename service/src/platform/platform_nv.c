#include "platform_internal.h"

static SOFTTPM_STATE_STORE* s_stateStore = NULL;
static int s_nvLoaded = 0;
static int s_nvNeedsManufacture = 0;

int platform_nv_configure(SOFTTPM_STATE_STORE* Store)
{
    if (Store == NULL)
    {
        return -1;
    }

    s_stateStore = Store;
    return 0;
}

void platform_nv_mark_manufactured(void)
{
    s_nvNeedsManufacture = 0;
}

LIB_EXPORT void _plat__NvErrors(int recoverable, int unrecoverable)
{
    s_NV_unrecoverable = unrecoverable;
    s_NV_recoverable = recoverable;
}

LIB_EXPORT int _plat__NVEnable(void* platParameter, size_t paramSize)
{
    (void)platParameter;
    (void)paramSize;

    s_NV_unrecoverable = FALSE;
    s_NV_recoverable = FALSE;

    if (s_nvLoaded)
    {
        return 0;
    }

    if (s_stateStore == NULL)
    {
        log_error("platform-nv: no state store configured");
        return -1;
    }

    if (state_store_load_nv(
            s_stateStore,
            s_NV,
            NV_MEMORY_SIZE,
            &s_nvNeedsManufacture) != 0)
    {
        return -1;
    }

    s_nvLoaded = 1;
    s_NvIsAvailable = TRUE;

    if (s_NV_unrecoverable)
    {
        return -1;
    }

    return s_NV_recoverable;
}

LIB_EXPORT void _plat__NVDisable(void* platParameter, size_t paramSize)
{
    int deleteContents;

    (void)paramSize;

    deleteContents = ((intptr_t)platParameter != 0) ? TRUE : FALSE;

    if (deleteContents && s_stateStore != NULL)
    {
        state_store_reset(s_stateStore);
        memset(s_NV, 0xFF, sizeof(s_NV));
        s_nvNeedsManufacture = 1;
    }

    s_nvLoaded = 0;
    s_NvIsAvailable = FALSE;
}

LIB_EXPORT int _plat__GetNvReadyState(void)
{
    return s_NvIsAvailable ? NV_READY : NV_WRITEFAILURE;
}

LIB_EXPORT int _plat__NvMemoryRead(unsigned int startOffset, unsigned int size, void* data)
{
    if ((size_t)startOffset + size > NV_MEMORY_SIZE)
    {
        return FALSE;
    }

    memcpy(data, &s_NV[startOffset], size);
    return TRUE;
}

LIB_EXPORT int _plat__NvGetChangedStatus(unsigned int startOffset, unsigned int size, void* data)
{
    if ((size_t)startOffset + size > NV_MEMORY_SIZE)
    {
        return NV_INVALID_LOCATION;
    }

    return memcmp(&s_NV[startOffset], data, size) != 0 ? NV_HAS_CHANGED : NV_IS_SAME;
}

LIB_EXPORT int _plat__NvMemoryWrite(unsigned int startOffset, unsigned int size, void* data)
{
    if ((size_t)startOffset + size > NV_MEMORY_SIZE)
    {
        return FALSE;
    }

    memcpy(&s_NV[startOffset], data, size);
    return TRUE;
}

LIB_EXPORT int _plat__NvMemoryClear(unsigned int startOffset, unsigned int size)
{
    if ((size_t)startOffset + size > NV_MEMORY_SIZE)
    {
        return FALSE;
    }

    memset(&s_NV[startOffset], 0xFF, size);
    return TRUE;
}

LIB_EXPORT int _plat__NvMemoryMove(unsigned int sourceOffset, unsigned int destOffset, unsigned int size)
{
    if ((size_t)sourceOffset + size > NV_MEMORY_SIZE
        || (size_t)destOffset + size > NV_MEMORY_SIZE)
    {
        return FALSE;
    }

    memmove(&s_NV[destOffset], &s_NV[sourceOffset], size);
    return TRUE;
}

LIB_EXPORT int _plat__NvCommit(void)
{
    if (s_stateStore == NULL)
    {
        return 1;
    }

    if (state_store_save_nv(
            s_stateStore,
            s_NV,
            NV_MEMORY_SIZE,
            !s_nvNeedsManufacture) != 0)
    {
        log_error("platform-nv: cannot persist TPM state");
        return 1;
    }

    return 0;
}

LIB_EXPORT void _plat__TearDown(void)
{
    if (s_stateStore != NULL)
    {
        state_store_reset(s_stateStore);
    }

    memset(s_NV, 0, sizeof(s_NV));
    s_nvNeedsManufacture = 1;
    s_nvLoaded = 0;
    s_NvIsAvailable = FALSE;
}

LIB_EXPORT void _plat__SetNvAvail(void)
{
    s_NvIsAvailable = TRUE;
}

LIB_EXPORT void _plat__ClearNvAvail(void)
{
    s_NvIsAvailable = FALSE;
}

LIB_EXPORT int _plat__NVNeedsManufacture(void)
{
    return s_nvNeedsManufacture;
}
