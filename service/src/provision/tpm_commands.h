#ifndef SOFTTPM_TPM_COMMANDS_H
#define SOFTTPM_TPM_COMMANDS_H

#include <stdint.h>

#define SOFTTPM_TPM_CREATE_PRIMARY 0x00000131
#define SOFTTPM_TPM_PCR_EXTEND 0x00000182
#define SOFTTPM_TPM_EVICT_CONTROL 0x00000120
#define SOFTTPM_TPM_NV_DEFINE_SPACE 0x0000012A
#define SOFTTPM_TPM_NV_WRITE 0x00000137
#define SOFTTPM_TPM_NV_READ_PUBLIC 0x00000169
#define SOFTTPM_TPM_READ_PUBLIC 0x00000173

#define SOFTTPM_TPM_ALG_RSA 0x0001
#define SOFTTPM_TPM_ALG_SHA256 0x000B
#define SOFTTPM_TPM_ALG_AES 0x0006
#define SOFTTPM_TPM_ALG_CFB 0x0043
#define SOFTTPM_TPM_ALG_NULL 0x0010

#define SOFTTPM_TPM_RH_OWNER 0x40000001
#define SOFTTPM_TPM_RH_ENDORSEMENT 0x4000000B
#define SOFTTPM_TPM_RH_PLATFORM 0x4000000C
#define SOFTTPM_TPM_RS_PW 0x40000009

#define SOFTTPM_TPM_RC_SUCCESS 0x000

#define SOFTTPM_EK_PERSISTENT_HANDLE 0x81010001
#define SOFTTPM_SRK_PERSISTENT_HANDLE 0x81000001
#define SOFTTPM_EK_CERT_NV_INDEX 0x01C00002

#define SOFTTPM_NV_ATTRIBUTES_PPWRITE 0x00000001
#define SOFTTPM_NV_ATTRIBUTES_OWNERREAD 0x00020000
#define SOFTTPM_NV_ATTRIBUTES_AUTHREAD 0x00040000
#define SOFTTPM_NV_ATTRIBUTES_PLATFORMCREATE 0x40000000
#define SOFTTPM_EK_CERT_NV_ATTRIBUTES \
    (SOFTTPM_NV_ATTRIBUTES_PPWRITE | SOFTTPM_NV_ATTRIBUTES_OWNERREAD \
     | SOFTTPM_NV_ATTRIBUTES_AUTHREAD | SOFTTPM_NV_ATTRIBUTES_PLATFORMCREATE)

#define SOFTTPM_MAX_NV_WRITE_CHUNK 1024

uint32_t tpm_be32_get(const uint8_t* Buffer);
void tpm_be16_put(uint8_t* Buffer, uint16_t Value);
void tpm_be32_put(uint8_t* Buffer, uint32_t Value);

uint32_t tpm_response_code(const uint8_t* Response, uint32_t ResponseSize);
int tpm_response_is_success(const uint8_t* Response, uint32_t ResponseSize);
int tpm_response_parameter(
    const uint8_t* Response,
    uint32_t ResponseSize,
    const uint8_t** Parameter,
    uint32_t* ParameterSize);

int tpm_response_create_primary(
    const uint8_t* Response,
    uint32_t ResponseSize,
    uint32_t* ObjectHandle,
    const uint8_t** PublicBlob,
    uint32_t* PublicBlobSize);

uint32_t tpm_build_read_public(
    uint32_t ObjectHandle,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_nv_read_public(
    uint32_t NvIndex,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_create_primary(
    uint32_t PrimaryHandle,
    const uint8_t* PublicArea,
    uint32_t PublicAreaSize,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_pcr_extend(
    uint32_t PcrHandle,
    const uint8_t* DigestValues,
    uint32_t DigestValuesSize,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_evict_control(
    uint32_t AuthHandle,
    uint32_t ObjectHandle,
    uint32_t PersistentHandle,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_nv_define_space(
    uint32_t AuthHandle,
    uint32_t NvIndex,
    uint16_t NameAlg,
    uint32_t Attributes,
    uint16_t DataSize,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_nv_write(
    uint32_t AuthHandle,
    uint32_t NvIndex,
    uint16_t Offset,
    const uint8_t* Data,
    uint16_t DataSize,
    uint8_t* Buffer,
    uint32_t BufferSize);

uint32_t tpm_build_rsa_public_area(
    uint32_t ObjectAttributes,
    const uint8_t* AuthPolicy,
    uint16_t AuthPolicySize,
    uint8_t* Buffer,
    uint32_t BufferSize);

int tpm_public_extract_rsa_modulus(
    const uint8_t* PublicArea,
    uint32_t PublicAreaSize,
    const uint8_t** Modulus,
    uint32_t* ModulusSize);

uint32_t tpm_public_size_of(const uint8_t* PublicArea, uint32_t AreaSize);

#endif
