
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "platform_wbcl.h"
#include "platform_log.h"
#include "log.h"

#define SOFTTPM_WBCL_REG_PATH L"SYSTEM\\CurrentControlSet\\Control\\IntegrityServices"
#define SOFTTPM_WBCL_VALUE_NAME L"WBCL"

#define SOFTTPM_WBCL_EVENT_NO_ACTION 0x00000003u
#define SOFTTPM_WBCL_ALG_SHA256 0x000Bu
#define SOFTTPM_WBCL_ALG_SHA384 0x000Cu

typedef struct
{
    unsigned char Data[96];
    size_t Size;
} SOFTTPM_WBCL_BUFFER;

static const unsigned char s_zeroDigest[20] = {0};

static void SoftTpmWbclAppendU8(SOFTTPM_WBCL_BUFFER* Buffer, unsigned char Value)
{
    if (Buffer->Size + 1 <= sizeof(Buffer->Data))
    {
        Buffer->Data[Buffer->Size++] = Value;
    }
}

static void SoftTpmWbclAppendU16(SOFTTPM_WBCL_BUFFER* Buffer, uint16_t Value)
{
    SoftTpmWbclAppendU8(Buffer, (unsigned char)(Value & 0xFF));
    SoftTpmWbclAppendU8(Buffer, (unsigned char)((Value >> 8) & 0xFF));
}

static void SoftTpmWbclAppendU32(SOFTTPM_WBCL_BUFFER* Buffer, uint32_t Value)
{
    SoftTpmWbclAppendU8(Buffer, (unsigned char)(Value & 0xFF));
    SoftTpmWbclAppendU8(Buffer, (unsigned char)((Value >> 8) & 0xFF));
    SoftTpmWbclAppendU8(Buffer, (unsigned char)((Value >> 16) & 0xFF));
    SoftTpmWbclAppendU8(Buffer, (unsigned char)((Value >> 24) & 0xFF));
}

static void SoftTpmWbclAppendBytes(
    SOFTTPM_WBCL_BUFFER* Buffer,
    const void* Data,
    size_t DataSize)
{
    if (Buffer->Size + DataSize <= sizeof(Buffer->Data))
    {
        memcpy(Buffer->Data + Buffer->Size, Data, DataSize);
        Buffer->Size += DataSize;
    }
}

static void SoftTpmWbclPatchU32(
    SOFTTPM_WBCL_BUFFER* Buffer,
    size_t Offset,
    uint32_t Value)
{
    if (Offset + 4 > sizeof(Buffer->Data))
    {
        return;
    }

    Buffer->Data[Offset] = (unsigned char)(Value & 0xFF);
    Buffer->Data[Offset + 1] = (unsigned char)((Value >> 8) & 0xFF);
    Buffer->Data[Offset + 2] = (unsigned char)((Value >> 16) & 0xFF);
    Buffer->Data[Offset + 3] = (unsigned char)((Value >> 24) & 0xFF);
}

static int SoftTpmWbclBuild(SOFTTPM_WBCL_BUFFER* Buffer)
{
    size_t eventSizeOffset;
    size_t eventStart;

    memset(Buffer, 0, sizeof(*Buffer));

    SoftTpmWbclAppendU32(Buffer, 0);
    SoftTpmWbclAppendU32(Buffer, SOFTTPM_WBCL_EVENT_NO_ACTION);
    SoftTpmWbclAppendBytes(Buffer, s_zeroDigest, sizeof(s_zeroDigest));
    eventSizeOffset = Buffer->Size;
    SoftTpmWbclAppendU32(Buffer, 0);
    eventStart = Buffer->Size;

    SoftTpmWbclAppendBytes(Buffer, "Spec ID Event03", sizeof("Spec ID Event03"));
    SoftTpmWbclAppendU32(Buffer, 0);
    SoftTpmWbclAppendU8(Buffer, 0);
    SoftTpmWbclAppendU8(Buffer, 2);
    SoftTpmWbclAppendU8(Buffer, 0);
    SoftTpmWbclAppendU8(Buffer, 2);
    SoftTpmWbclAppendU32(Buffer, 2);
    SoftTpmWbclAppendU16(Buffer, SOFTTPM_WBCL_ALG_SHA256);
    SoftTpmWbclAppendU16(Buffer, 32);
    SoftTpmWbclAppendU16(Buffer, SOFTTPM_WBCL_ALG_SHA384);
    SoftTpmWbclAppendU16(Buffer, 48);
    SoftTpmWbclAppendU8(Buffer, 0);

    SoftTpmWbclPatchU32(
        Buffer,
        eventSizeOffset,
        (uint32_t)(Buffer->Size - eventStart));

    if (Buffer->Size < 65)
    {
        return -1;
    }

    return 0;
}

int platform_wbcl_publish(void)
{
    SOFTTPM_WBCL_BUFFER buffer;
    const unsigned char* platformData;
    uint32_t platformDataSize = 0;
    const unsigned char* valueData;
    uint32_t valueSize;
    HKEY key = NULL;
    DWORD disposition = 0;
    LONG status;

    platformData = platform_log_data(&platformDataSize);

    if (platformData != NULL)
    {
        valueData = platformData;
        valueSize = platformDataSize;
    }
    else
    {
        if (SoftTpmWbclBuild(&buffer) != 0)
        {
            log_warn("platform: cannot build the synthetic WBCL");
            return -1;
        }

        valueData = buffer.Data;
        valueSize = (uint32_t)buffer.Size;
    }

    status = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        SOFTTPM_WBCL_REG_PATH,
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY,
        NULL,
        &key,
        &disposition);

    if (status != ERROR_SUCCESS)
    {
        log_warn("platform: cannot open %ls (error %ld)", SOFTTPM_WBCL_REG_PATH, status);
        return -1;
    }

    status = RegSetValueExW(
        key,
        SOFTTPM_WBCL_VALUE_NAME,
        0,
        REG_BINARY,
        valueData,
        (DWORD)valueSize);

    RegCloseKey(key);

    if (status != ERROR_SUCCESS)
    {
        log_warn("platform: cannot publish the WBCL value (error %ld)", status);
        return -1;
    }

    if (platformData != NULL)
    {
        log_info("platform: published the platform log as WBCL (%u bytes) at %ls\\%ls",
                 (unsigned)valueSize,
                 SOFTTPM_WBCL_REG_PATH,
                 SOFTTPM_WBCL_VALUE_NAME);
    }
    else
    {
        log_info("platform: published synthetic WBCL (%u bytes) at %ls\\%ls",
                 (unsigned)valueSize,
                 SOFTTPM_WBCL_REG_PATH,
                 SOFTTPM_WBCL_VALUE_NAME);
    }

    return 0;
}

int platform_wbcl_remove(void)
{
    HKEY key = NULL;
    LONG status;

    status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        SOFTTPM_WBCL_REG_PATH,
        0,
        KEY_SET_VALUE | KEY_WOW64_64KEY,
        &key);

    if (status != ERROR_SUCCESS)
    {
        return 0;
    }

    status = RegDeleteValueW(key, SOFTTPM_WBCL_VALUE_NAME);
    RegCloseKey(key);

    if (status == ERROR_SUCCESS)
    {
        log_info("platform: removed the synthetic WBCL value");
    }
    else if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
    {
        log_warn("platform: cannot remove the WBCL value (error %ld)", status);
    }

    return 0;
}
