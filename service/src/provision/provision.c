
#include <windows.h>
#include <string.h>
#include "provision.h"
#include "tpm_commands.h"
#include "ek_certificate.h"
#include "engine.h"
#include "log.h"
#include "platform/platform_internal.h"

#define SOFTTPM_PROVISION_BUFFER_SIZE 4096
#define SOFTTPM_TPM_RC_NV_DEFINED 0x14C
#define SOFTTPM_PUBLIC_BLOB_MAX 512
#define SOFTTPM_EK_OBJECT_ATTRIBUTES 0x000300B2
#define SOFTTPM_SRK_OBJECT_ATTRIBUTES 0x00030472

static const unsigned char s_ekAuthPolicy[32] = {
    0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xb3, 0xf8, 0x1a, 0x90, 0xcc, 0x8d,
    0x46, 0xa5, 0xd7, 0x24, 0xfd, 0x52, 0xd7, 0x6e, 0x06, 0x52, 0x0b, 0x64,
    0xf2, 0xa1, 0xda, 0x1b, 0x33, 0x14, 0x69, 0xaa};

static uint16_t SoftTpmBe16(const unsigned char* Buffer)
{
    return (uint16_t)((Buffer[0] << 8) | Buffer[1]);
}

static uint32_t SoftTpmBe32(const unsigned char* Buffer)
{
    return ((uint32_t)Buffer[0] << 24) | ((uint32_t)Buffer[1] << 16)
           | ((uint32_t)Buffer[2] << 8) | Buffer[3];
}

static unsigned long s_certificateFaultCode;
static const void* s_certificateFaultAddress;

static int SoftTpmCertificateFaultFilter(EXCEPTION_POINTERS* Information)
{
    s_certificateFaultCode = Information->ExceptionRecord->ExceptionCode;
    s_certificateFaultAddress = Information->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int SoftTpmRunCommandQuiet(
    const unsigned char* Command,
    uint32_t CommandSize,
    unsigned char* Response,
    uint32_t* ResponseSize,
    uint32_t* ResponseCode)
{
    uint32_t responseSize = *ResponseSize;
    uint32_t commandCode = CommandSize >= 10 ? SoftTpmBe32(Command + 6) : 0;

    log_info(
        "provision: executing TPM command 0x%08X (%u bytes)",
        commandCode,
        CommandSize);

    if (engine_execute(Command, CommandSize, Response, &responseSize) != 0)
    {
        log_error("provision: TPM command 0x%08X did not execute", commandCode);
        return -1;
    }

    *ResponseSize = responseSize;
    *ResponseCode = tpm_response_code(Response, responseSize);

    log_info(
        "provision: TPM command 0x%08X responded (%u bytes, RC 0x%08X)",
        commandCode,
        responseSize,
        *ResponseCode);

    return 0;
}

static int SoftTpmRunProvisionCommand(
    const unsigned char* Command,
    uint32_t CommandSize,
    unsigned char* Response,
    uint32_t* ResponseSize,
    const char* What)
{
    uint32_t responseCode = 0;

    if (SoftTpmRunCommandQuiet(
            Command, CommandSize, Response, ResponseSize, &responseCode) != 0)
    {
        log_error("provision: %s did not run", What);
        return -1;
    }

    if (responseCode != SOFTTPM_TPM_RC_SUCCESS)
    {
        log_warn("provision: %s failed with TPM_RC 0x%08X", What, responseCode);
        return -1;
    }

    return 0;
}

static int SoftTpmProbeHandle(uint32_t Handle)
{
    unsigned char command[64];
    unsigned char response[SOFTTPM_PROVISION_BUFFER_SIZE];
    uint32_t commandSize;
    uint32_t responseSize = sizeof(response);
    uint32_t responseCode = 0;

    commandSize = tpm_build_read_public(Handle, command, sizeof(command));

    if (commandSize == 0)
    {
        return -1;
    }

    if (SoftTpmRunCommandQuiet(
            command, commandSize, response, &responseSize, &responseCode) != 0)
    {
        return -1;
    }

    return responseCode == SOFTTPM_TPM_RC_SUCCESS ? 1 : 0;
}

static int SoftTpmProbeNvIndex(uint32_t NvIndex)
{
    unsigned char command[64];
    unsigned char response[SOFTTPM_PROVISION_BUFFER_SIZE];
    uint32_t commandSize;
    uint32_t responseSize = sizeof(response);
    uint32_t responseCode = 0;

    commandSize = tpm_build_nv_read_public(NvIndex, command, sizeof(command));

    if (commandSize == 0)
    {
        return -1;
    }

    if (SoftTpmRunCommandQuiet(
            command, commandSize, response, &responseSize, &responseCode) != 0)
    {
        return -1;
    }

    return responseCode == SOFTTPM_TPM_RC_SUCCESS ? 1 : 0;
}

static int SoftTpmReadPublicBlob(
    uint32_t Handle,
    unsigned char* Blob,
    uint32_t* BlobSize)
{
    unsigned char command[64];
    unsigned char response[SOFTTPM_PROVISION_BUFFER_SIZE];
    const unsigned char* parameter;
    uint32_t parameterSize;
    uint32_t commandSize;
    uint32_t responseSize = sizeof(response);
    uint16_t areaSize;

    commandSize = tpm_build_read_public(Handle, command, sizeof(command));

    if (commandSize == 0)
    {
        return -1;
    }

    if (SoftTpmRunProvisionCommand(
            command, commandSize, response, &responseSize, "ReadPublic") != 0)
    {
        return -1;
    }

    if (tpm_response_parameter(response, responseSize, &parameter, &parameterSize) != 0
        || parameterSize < 2)
    {
        return -1;
    }

    areaSize = SoftTpmBe16(parameter);

    if ((uint32_t)areaSize + 2 > parameterSize || (uint32_t)areaSize + 2 > *BlobSize)
    {
        return -1;
    }

    memcpy(Blob, parameter, (size_t)areaSize + 2);
    *BlobSize = (uint32_t)areaSize + 2;
    return 0;
}

static int SoftTpmCreateAndPersist(
    uint32_t PrimaryHandle,
    const unsigned char* PublicArea,
    uint32_t PublicAreaSize,
    uint32_t PersistentHandle,
    unsigned char* OutPublicBlob,
    uint32_t* OutPublicBlobSize)
{
    unsigned char command[SOFTTPM_PROVISION_BUFFER_SIZE];
    unsigned char response[SOFTTPM_PROVISION_BUFFER_SIZE];
    const unsigned char* publicBlob;
    uint32_t publicBlobSize;
    uint32_t commandSize;
    uint32_t responseSize;
    uint32_t transientHandle;

    commandSize = tpm_build_create_primary(
        PrimaryHandle, PublicArea, PublicAreaSize, command, sizeof(command));

    if (commandSize == 0)
    {
        return -1;
    }

    responseSize = sizeof(response);

    if (SoftTpmRunProvisionCommand(
            command, commandSize, response, &responseSize, "CreatePrimary") != 0)
    {
        return -1;
    }

    if (tpm_response_create_primary(
            response, responseSize, &transientHandle, &publicBlob, &publicBlobSize) != 0)
    {
        log_warn("provision: CreatePrimary response is malformed (%u bytes)", responseSize);
        return -1;
    }

    if (OutPublicBlob != NULL)
    {
        if (publicBlobSize > *OutPublicBlobSize)
        {
            log_warn(
                "provision: public blob is too large (%u > %u)",
                publicBlobSize,
                *OutPublicBlobSize);
            return -1;
        }

        memcpy(OutPublicBlob, publicBlob, publicBlobSize);
        *OutPublicBlobSize = publicBlobSize;
    }

    commandSize = tpm_build_evict_control(
        SOFTTPM_TPM_RH_OWNER,
        transientHandle,
        PersistentHandle,
        command,
        sizeof(command));

    if (commandSize == 0)
    {
        return -1;
    }

    responseSize = sizeof(response);

    return SoftTpmRunProvisionCommand(
        command, commandSize, response, &responseSize, "EvictControl");
}

static int SoftTpmWriteEkCertificate(
    const wchar_t* StateDirectory,
    const unsigned char* PublicBlob,
    uint32_t PublicBlobSize)
{
    const unsigned char* modulus;
    const unsigned char* area;
    uint32_t modulusSize;
    uint32_t areaSize;
    unsigned char* der = NULL;
    uint32_t derSize = 0;
    unsigned char command[SOFTTPM_PROVISION_BUFFER_SIZE];
    unsigned char response[SOFTTPM_PROVISION_BUFFER_SIZE];
    uint32_t commandSize;
    uint32_t responseSize;
    uint32_t responseCode;
    uint16_t offset;

    log_info("provision: issuing the EK certificate (%u-byte public blob)", PublicBlobSize);

    if (PublicBlobSize < 2)
    {
        return -1;
    }

    area = PublicBlob + 2;
    areaSize = SoftTpmBe16(PublicBlob);

    if (areaSize + 2 > PublicBlobSize)
    {
        return -1;
    }

    if (tpm_public_extract_rsa_modulus(area, areaSize, &modulus, &modulusSize) != 0)
    {
        log_error("provision: cannot extract the EK public key");
        return -1;
    }

    log_info("provision: EK modulus is %u bytes", modulusSize);

    if (ek_certificate_issue(StateDirectory, modulus, modulusSize, &der, &derSize) != 0)
    {
        log_error("provision: cannot issue the EK certificate");
        return -1;
    }

    if (derSize == 0 || derSize > 2048)
    {
        log_error("provision: EK certificate size %u is invalid", derSize);
        ek_certificate_free(der);
        return -1;
    }

    commandSize = tpm_build_nv_define_space(
        SOFTTPM_TPM_RH_PLATFORM,
        SOFTTPM_EK_CERT_NV_INDEX,
        SOFTTPM_TPM_ALG_SHA256,
        SOFTTPM_EK_CERT_NV_ATTRIBUTES,
        (uint16_t)derSize,
        command,
        sizeof(command));

    if (commandSize == 0)
    {
        ek_certificate_free(der);
        return -1;
    }

    responseSize = sizeof(response);

    if (SoftTpmRunCommandQuiet(
            command, commandSize, response, &responseSize, &responseCode) != 0)
    {
        log_error("provision: NV_DefineSpace did not run");
        ek_certificate_free(der);
        return -1;
    }

    if (responseCode != SOFTTPM_TPM_RC_SUCCESS)
    {
        if ((responseCode & 0xFFF) == SOFTTPM_TPM_RC_NV_DEFINED)
        {
            log_info("provision: EK certificate index already exists");
            ek_certificate_free(der);
            return 0;
        }

        log_warn("provision: NV_DefineSpace failed with TPM_RC 0x%08X", responseCode);
        ek_certificate_free(der);
        return -1;
    }

    for (offset = 0; offset < (uint16_t)derSize; offset += SOFTTPM_MAX_NV_WRITE_CHUNK)
    {
        uint16_t remaining = (uint16_t)(derSize - offset);
        uint16_t chunk = remaining > SOFTTPM_MAX_NV_WRITE_CHUNK
                             ? SOFTTPM_MAX_NV_WRITE_CHUNK
                             : remaining;

        commandSize = tpm_build_nv_write(
            SOFTTPM_TPM_RH_PLATFORM,
            SOFTTPM_EK_CERT_NV_INDEX,
            offset,
            der + offset,
            chunk,
            command,
            sizeof(command));

        if (commandSize == 0)
        {
            ek_certificate_free(der);
            return -1;
        }

        responseSize = sizeof(response);

        if (SoftTpmRunProvisionCommand(
                command, commandSize, response, &responseSize, "NV_Write") != 0)
        {
            ek_certificate_free(der);
            return -1;
        }
    }

    log_info("provision: EK certificate written (%u bytes)", derSize);
    ek_certificate_free(der);
    return 0;
}

int provision_run(const wchar_t* StateDirectory)
{
    unsigned char publicArea[128];
    unsigned char ekPublic[SOFTTPM_PUBLIC_BLOB_MAX];
    uint32_t publicAreaSize;
    uint32_t ekPublicSize = 0;
    int hasEk;
    int hasSrk;
    int hasCertificate;
    int result = -1;

    if (StateDirectory == NULL)
    {
        return -1;
    }

    log_info("provision: checking persistent objects");

    _plat__Signal_PhysicalPresenceOn();

    hasEk = SoftTpmProbeHandle(SOFTTPM_EK_PERSISTENT_HANDLE);
    hasSrk = SoftTpmProbeHandle(SOFTTPM_SRK_PERSISTENT_HANDLE);
    hasCertificate = SoftTpmProbeNvIndex(SOFTTPM_EK_CERT_NV_INDEX);

    log_info(
        "provision: probe results EK=%d SRK=%d certificate=%d",
        hasEk,
        hasSrk,
        hasCertificate);

    if (hasEk < 0)
    {
        goto cleanup;
    }

    if (hasEk > 0)
    {
        uint32_t blobSize = sizeof(ekPublic);

        if (SoftTpmReadPublicBlob(
                SOFTTPM_EK_PERSISTENT_HANDLE, ekPublic, &blobSize) == 0)
        {
            ekPublicSize = blobSize;
        }
    }

    if (hasEk == 0)
    {
        publicAreaSize = tpm_build_rsa_public_area(
            SOFTTPM_EK_OBJECT_ATTRIBUTES,
            s_ekAuthPolicy,
            (uint16_t)sizeof(s_ekAuthPolicy),
            publicArea,
            sizeof(publicArea));

        if (publicAreaSize == 0)
        {
            goto cleanup;
        }

        ekPublicSize = sizeof(ekPublic);
        if (SoftTpmCreateAndPersist(
                SOFTTPM_TPM_RH_ENDORSEMENT,
                publicArea,
                publicAreaSize,
                SOFTTPM_EK_PERSISTENT_HANDLE,
                ekPublic,
                &ekPublicSize) != 0)
        {
            goto cleanup;
        }

        log_info("provision: created EK at 0x%08X", SOFTTPM_EK_PERSISTENT_HANDLE);
    }

    if (hasSrk == 0)
    {
        publicAreaSize = tpm_build_rsa_public_area(
            SOFTTPM_SRK_OBJECT_ATTRIBUTES,
            NULL,
            0,
            publicArea,
            sizeof(publicArea));

        if (publicAreaSize == 0)
        {
            goto cleanup;
        }

        if (SoftTpmCreateAndPersist(
                SOFTTPM_TPM_RH_OWNER,
                publicArea,
                publicAreaSize,
                SOFTTPM_SRK_PERSISTENT_HANDLE,
                NULL,
                NULL) != 0)
        {
            goto cleanup;
        }

        log_info("provision: created SRK at 0x%08X", SOFTTPM_SRK_PERSISTENT_HANDLE);
    }

    if (hasCertificate == 0)
    {
        int certificateResult;

        if (ekPublicSize == 0)
        {
            uint32_t blobSize = sizeof(ekPublic);

            if (SoftTpmReadPublicBlob(
                    SOFTTPM_EK_PERSISTENT_HANDLE, ekPublic, &blobSize) != 0)
            {
                goto cleanup;
            }

            ekPublicSize = blobSize;
        }

        s_certificateFaultCode = 0;
        s_certificateFaultAddress = NULL;

        __try
        {
            certificateResult =
                SoftTpmWriteEkCertificate(StateDirectory, ekPublic, ekPublicSize);
        }
        __except (SoftTpmCertificateFaultFilter(GetExceptionInformation()))
        {
            log_error(
                "provision: EK certificate step crashed with exception 0x%08X at 0x%p",
                s_certificateFaultCode,
                s_certificateFaultAddress);
            certificateResult = -1;
        }

        if (certificateResult != 0)
        {
            goto cleanup;
        }
    }

    result = 0;
    log_info("provision: TPM objects are ready");

cleanup:
    _plat__Signal_PhysicalPresenceOff();
    return result;
}
