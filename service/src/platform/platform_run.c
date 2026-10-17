
#include <setjmp.h>
#include "platform_internal.h"

static jmp_buf s_jumpBuffer;

extern BOOL g_inFailureMode;
#if ALLOW_FORCE_FAILURE_MODE
extern BOOL g_forceFailureMode;
#endif
#if FAIL_TRACE
extern const char* s_failFunctionName;
#endif
extern UINT32 s_failFunction;
extern UINT32 s_failLine;
extern UINT32 s_failCode;

LIB_EXPORT void _plat__RunCommand(
    uint32_t requestSize,
    unsigned char* request,
    uint32_t* responseSize,
    unsigned char** response)
{
    uint32_t volatile savedSize = *responseSize;
    unsigned char* volatile savedResponse = *response;

    if (setjmp(s_jumpBuffer) == 0)
    {
        ExecuteCommand(requestSize, request, responseSize, response);
    }
    else
    {
        *responseSize = savedSize;
        *response = savedResponse;
        ExecuteCommand(requestSize, request, responseSize, response);
    }
}

LIB_EXPORT NORETURN void _plat__Fail(void)
{
#if FAIL_TRACE
    log_error(
        "platform: TPM failure mode entered in %s (line %u, code 0x%08X)",
        s_failFunctionName != NULL ? s_failFunctionName : "?",
        s_failLine,
        s_failCode);
#else
    log_error(
        "platform: TPM failure mode entered (function %u, line %u, code 0x%08X)",
        s_failFunction,
        s_failLine,
        s_failCode);
#endif

#if ALLOW_FORCE_FAILURE_MODE
    g_forceFailureMode = FALSE;
#endif

    longjmp(s_jumpBuffer, 1);
}
