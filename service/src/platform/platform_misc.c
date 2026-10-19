
#include <windows.h>
#include <bcrypt.h>
#include "platform_internal.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

extern int32_t _plat__GetEntropy(unsigned char* entropy, uint32_t amount);

#define SOFTTPM_MANUFACTURER "STPM"
#define SOFTTPM_VENDOR_STRING_1 "Soft"
#define SOFTTPM_VENDOR_STRING_2 "TPM\0"
#define SOFTTPM_VENDOR_STRING_3 "\0\0\0\0"
#define SOFTTPM_VENDOR_STRING_4 "\0\0\0\0"
#define SOFTTPM_FIRMWARE_V1 0x20260101
#define SOFTTPM_FIRMWARE_V2 0x00010000
#define SOFTTPM_MAX_SVN 255
#define SOFTTPM_CURRENT_SVN 10

LIB_EXPORT int _plat__IsCanceled(void)
{
    return s_isCanceled;
}

LIB_EXPORT void _plat__SetCancel(void)
{
    s_isCanceled = TRUE;
}

LIB_EXPORT void _plat__ClearCancel(void)
{
    s_isCanceled = FALSE;
}

LIB_EXPORT unsigned char _plat__LocalityGet(void)
{
    return s_locality;
}

LIB_EXPORT void _plat__LocalitySet(unsigned char locality)
{
    if (locality > 4 && locality < 32)
    {
        locality = 0;
    }

    s_locality = locality;
}

LIB_EXPORT int _plat__PhysicalPresenceAsserted(void)
{
    return s_physicalPresence;
}

LIB_EXPORT void _plat__Signal_PhysicalPresenceOn(void)
{
    s_physicalPresence = TRUE;
}

LIB_EXPORT void _plat__Signal_PhysicalPresenceOff(void)
{
    s_physicalPresence = FALSE;
}

LIB_EXPORT int _plat__Signal_PowerOn(void)
{
    _plat__TimerReset();
    s_powerLost = TRUE;
    return 0;
}

LIB_EXPORT int _plat__WasPowerLost(void)
{
    int retVal = s_powerLost;
    s_powerLost = FALSE;
    return retVal;
}

LIB_EXPORT int _plat__Signal_Reset(void)
{
    s_locality = 0;
    s_isCanceled = FALSE;

    _TPM_Init();

    return 0;
}

LIB_EXPORT void _plat__Signal_PowerOff(void)
{
    _plat__NVDisable((void*)FALSE, 0);

#if ACT_SUPPORT
    _plat__ACT_EnableTicks(FALSE);
#endif
}

LIB_EXPORT int32_t _plat__GetEntropy(unsigned char* entropy, uint32_t amount)
{
    NTSTATUS status;
    uint32_t rndNum;

    if (amount == 0)
    {
        return 0;
    }

    status = BCryptGenRandom(
        NULL,
        entropy,
        amount,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (!NT_SUCCESS(status))
    {
        return -1;
    }

    if (amount >= sizeof(rndNum))
    {
        memcpy(&rndNum, entropy, sizeof(rndNum));
        lastEntropy = rndNum;
    }

    return (int32_t)amount;
}

static uint32_t SoftTpmStringToUint32(const char s[4])
{
    const unsigned char* b = (const unsigned char*)s;
    return (((uint32_t)b[0] << 8 | b[1]) << 8 | b[2]) << 8 | b[3];
}

LIB_EXPORT uint32_t _plat__GetManufacturerCapabilityCode(void)
{
    return SoftTpmStringToUint32(SOFTTPM_MANUFACTURER);
}

LIB_EXPORT uint32_t _plat__GetVendorCapabilityCode(int index)
{
    switch (index)
    {
    case 1:
        return SoftTpmStringToUint32(SOFTTPM_VENDOR_STRING_1);
    case 2:
        return SoftTpmStringToUint32(SOFTTPM_VENDOR_STRING_2);
    case 3:
        return SoftTpmStringToUint32(SOFTTPM_VENDOR_STRING_3);
    case 4:
        return SoftTpmStringToUint32(SOFTTPM_VENDOR_STRING_4);
    default:
        return 0;
    }
}

LIB_EXPORT uint32_t _plat__GetTpmFirmwareVersionHigh(void)
{
    return SOFTTPM_FIRMWARE_V1;
}

LIB_EXPORT uint32_t _plat__GetTpmFirmwareVersionLow(void)
{
    return SOFTTPM_FIRMWARE_V2;
}

LIB_EXPORT uint16_t _plat__GetTpmFirmwareSvn(void)
{
    return SOFTTPM_CURRENT_SVN;
}

LIB_EXPORT uint16_t _plat__GetTpmFirmwareMaxSvn(void)
{
    return SOFTTPM_MAX_SVN;
}

#if SVN_LIMITED_SUPPORT
LIB_EXPORT int _plat__GetTpmFirmwareSvnSecret(
    uint16_t svn,
    uint16_t secret_buf_size,
    uint8_t* secret_buf,
    uint16_t* secret_size)
{
    uint16_t index;

    if (svn > SOFTTPM_CURRENT_SVN)
    {
        return -1;
    }

    for (index = 0; index < secret_buf_size; ++index)
    {
        secret_buf[index] = ((uint8_t*)&svn)[index % sizeof(svn)];
    }

    *secret_size = secret_buf_size;
    return 0;
}
#endif

#if FW_LIMITED_SUPPORT
LIB_EXPORT int _plat__GetTpmFirmwareSecret(
    uint16_t secret_buf_size,
    uint8_t* secret_buf,
    uint16_t* secret_size)
{
    uint32_t firmware = SOFTTPM_FIRMWARE_V2;
    uint16_t index;

    for (index = 0; index < secret_buf_size; ++index)
    {
        secret_buf[index] = ((uint8_t*)&firmware)[index % sizeof(firmware)];
    }

    *secret_size = secret_buf_size;
    return 0;
}
#endif

LIB_EXPORT uint32_t _plat__GetTpmType(void)
{
    return 1;
}

LIB_EXPORT void _plat__GetPlatformManufactureData(
    uint8_t* pPlatformPersistentData,
    uint32_t bufferSize)
{
    static const char prefix[] = "SoftTpm.PlatformData.v1";
    unsigned char buffer[sizeof(prefix) - 1 + SOFTTPM_STATE_KEY_SIZE];
    unsigned char digest[SOFTTPM_STATE_KEY_SIZE];
    uint32_t index;

    if (bufferSize == 0 || pPlatformPersistentData == NULL)
    {
        return;
    }

    if (softtpm_machine_entropy(digest) != 0)
    {
        memset(pPlatformPersistentData, 0xFF, bufferSize);
        return;
    }

    memcpy(buffer, prefix, sizeof(prefix) - 1);
    memcpy(buffer + sizeof(prefix) - 1, digest, sizeof(digest));

    if (softtpm_sha256(buffer, sizeof(buffer), digest) != 0)
    {
        memset(pPlatformPersistentData, 0xFF, bufferSize);
        return;
    }

    for (index = 0; index < bufferSize; ++index)
    {
        pPlatformPersistentData[index] = digest[index % sizeof(digest)];
    }
}
