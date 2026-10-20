
#include <windows.h>
#include "engine.h"
#include "log.h"
#include "platform/platform_internal.h"
#include <SoftTpmProtocol.h>

static SOFTTPM_STATE_STORE s_store;
static int s_running = 0;

static unsigned char s_commandBuffer[SOFTTPM_MAX_PACKET_SIZE];
static unsigned char s_responseBuffer[SOFTTPM_MAX_PACKET_SIZE];

static const unsigned char s_startupClear[] = {
    0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x44, 0x00, 0x00};

static const unsigned char s_shutdownClear[] = {
    0x80, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x01, 0x45, 0x00, 0x00};

static void SoftTpmSynthesizeFailure(unsigned char* Response, uint32_t* ResponseSize)
{
    static const unsigned char failure[] = {
        0x80, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x01, 0x01};

    uint32_t copySize = (uint32_t)sizeof(failure);

    if (*ResponseSize < copySize)
    {
        copySize = *ResponseSize;
    }

    if (copySize > 0)
    {
        memcpy(Response, failure, copySize);
    }

    *ResponseSize = (uint32_t)sizeof(failure);
}

static int SoftTpmRunCommandAtLocality(
    const unsigned char* Command,
    uint32_t CommandSize,
    uint32_t Locality,
    unsigned char* Response,
    uint32_t* ResponseSize)
{
    unsigned char* responsePointer = s_responseBuffer;
    uint32_t responseSize = sizeof(s_responseBuffer);
    unsigned char previousLocality;

    if (CommandSize > sizeof(s_commandBuffer))
    {
        log_error("engine: command of %u bytes exceeds packet limit", CommandSize);
        SoftTpmSynthesizeFailure(Response, ResponseSize);
        return -1;
    }

    if (Locality > SOFTTPM_MAX_LOCALITY)
    {
        log_error("engine: unsupported locality %u", Locality);
        SoftTpmSynthesizeFailure(Response, ResponseSize);
        return -1;
    }

    memcpy(s_commandBuffer, Command, CommandSize);

    previousLocality = _plat__LocalityGet();

    if (Locality != 0)
    {
        log_info("engine: command at locality %u", Locality);
    }

    _plat__LocalitySet((unsigned char)Locality);
    _plat__RunCommand(CommandSize, s_commandBuffer, &responseSize, &responsePointer);
    _plat__LocalitySet(previousLocality);

    if (responsePointer == NULL || responseSize > sizeof(s_responseBuffer))
    {
        log_error("engine: TPM produced an invalid response (%u bytes)", responseSize);
        SoftTpmSynthesizeFailure(Response, ResponseSize);
        return -1;
    }

    memcpy(Response, responsePointer, responseSize);
    *ResponseSize = responseSize;

    return 0;
}

static int SoftTpmRunCommand(
    const unsigned char* Command,
    uint32_t CommandSize,
    unsigned char* Response,
    uint32_t* ResponseSize)
{
    return SoftTpmRunCommandAtLocality(
        Command,
        CommandSize,
        0,
        Response,
        ResponseSize);
}

static int SoftTpmStartupClear(void)
{
    unsigned char response[64];
    uint32_t responseSize = sizeof(response);

    if (SoftTpmRunCommand(
            s_startupClear,
            (uint32_t)sizeof(s_startupClear),
            response,
            &responseSize) != 0)
    {
        return -1;
    }

    if (responseSize < 10)
    {
        log_error("engine: truncated TPM2_Startup response");
        return -1;
    }

    if (response[6] != 0 || response[7] != 0 || response[8] != 0 || response[9] != 0)
    {
        log_error(
            "engine: TPM2_Startup failed with code 0x%02X%02X%02X%02X",
            response[6],
            response[7],
            response[8],
            response[9]);
        return -1;
    }

    return 0;
}

static int SoftTpmManufacture(void)
{
    int rc = TPM_Manufacture(MANUF_FIRST_TIME);

    if (rc != MANUF_OK && rc != MANUF_ALREADY_DONE)
    {
        log_error("engine: TPM_Manufacture failed with %d", rc);
        return -1;
    }

    platform_nv_mark_manufactured();

    if (_plat__NvCommit() != 0)
    {
        return -1;
    }

    return 0;
}

int engine_start(const wchar_t* StateDirectory)
{
    int rc;

    if (s_running)
    {
        return 0;
    }

    if (StateDirectory == NULL)
    {
        log_error("engine: no state directory configured");
        return -1;
    }

    if (state_store_open(&s_store, StateDirectory) != 0)
    {
        return -1;
    }

    if (platform_nv_configure(&s_store) != 0)
    {
        return -1;
    }

    rc = _plat__NVEnable(NULL, 0);
    if (rc < 0)
    {
        log_error("engine: cannot load persistent TPM state");
        return -1;
    }

    if (_plat__NVNeedsManufacture())
    {
        log_info("engine: manufacturing a new TPM");

        if (SoftTpmManufacture() != 0)
        {
            return -1;
        }

        _plat__NVDisable((void*)FALSE, 0);
    }

    log_info("engine: TPM state sequence %u loaded from %ls",
             s_store.Sequence, state_store_directory(&s_store));

    if (_plat__ACT_Initialize() != TRUE)
    {
        log_error("engine: ACT initialization failed");
        return -1;
    }

    if (_plat__Signal_PowerOn() != 0)
    {
        log_error("engine: power-on failed");
        return -1;
    }

    if (_plat__Signal_Reset() != 0)
    {
        log_error("engine: reset failed");
        return -1;
    }

    if (SoftTpmStartupClear() != 0)
    {
        return -1;
    }

    s_running = 1;
    log_info("engine: TPM 2.0 engine started");

    return 0;
}

void engine_stop(void)
{
    unsigned char response[64];
    uint32_t responseSize = sizeof(response);

    if (!s_running)
    {
        return;
    }

    if (SoftTpmRunCommand(
            s_shutdownClear,
            (uint32_t)sizeof(s_shutdownClear),
            response,
            &responseSize) != 0)
    {
        log_warn("engine: TPM2_Shutdown(CLEAR) did not complete");
    }

    _plat__Signal_PowerOff();
    s_running = 0;

    log_info("engine: TPM 2.0 engine stopped");
}

int engine_execute(
    const unsigned char* Command,
    uint32_t CommandSize,
    unsigned char* Response,
    uint32_t* ResponseSize)
{
    return engine_execute_locality(
        Command,
        CommandSize,
        0,
        Response,
        ResponseSize);
}

int engine_execute_locality(
    const unsigned char* Command,
    uint32_t CommandSize,
    uint32_t Locality,
    unsigned char* Response,
    uint32_t* ResponseSize)
{
    if (Command == NULL || Response == NULL || ResponseSize == NULL)
    {
        return -1;
    }

    if (CommandSize < 10)
    {
        SoftTpmSynthesizeFailure(Response, ResponseSize);
        return -1;
    }

    if (!s_running)
    {
        SoftTpmSynthesizeFailure(Response, ResponseSize);
        return -1;
    }

    return SoftTpmRunCommandAtLocality(
        Command,
        CommandSize,
        Locality,
        Response,
        ResponseSize);
}
