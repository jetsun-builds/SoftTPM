#include <string.h>
#include "tpm_commands.h"

#define SOFTTPM_TPM_ST_NO_SESSIONS 0x8001
#define SOFTTPM_TPM_ST_SESSIONS 0x8002
#define SOFTTPM_PASSWORD_SESSION_SIZE 9

typedef struct
{
    uint8_t* Buffer;
    uint32_t Capacity;
    uint32_t Offset;
    int Overflow;
} TPM_WRITER;

static int WriterNeed(TPM_WRITER* Writer, uint32_t Size)
{
    if (Writer->Overflow || Writer->Offset + Size > Writer->Capacity)
    {
        Writer->Overflow = 1;
        return 0;
    }

    return 1;
}

static void WriterInit(TPM_WRITER* Writer, uint8_t* Buffer, uint32_t Capacity)
{
    Writer->Buffer = Buffer;
    Writer->Capacity = Capacity;
    Writer->Offset = 0;
    Writer->Overflow = 0;
}

static void WriterBytes(TPM_WRITER* Writer, const void* Data, uint32_t Size)
{
    if (!WriterNeed(Writer, Size))
    {
        return;
    }

    memcpy(Writer->Buffer + Writer->Offset, Data, Size);
    Writer->Offset += Size;
}

static void WriterU8(TPM_WRITER* Writer, uint8_t Value)
{
    WriterBytes(Writer, &Value, 1);
}

static void WriterU16(TPM_WRITER* Writer, uint16_t Value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(Value >> 8);
    bytes[1] = (uint8_t)Value;
    WriterBytes(Writer, bytes, 2);
}

static void WriterU32(TPM_WRITER* Writer, uint32_t Value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(Value >> 24);
    bytes[1] = (uint8_t)(Value >> 16);
    bytes[2] = (uint8_t)(Value >> 8);
    bytes[3] = (uint8_t)Value;
    WriterBytes(Writer, bytes, 4);
}

static void WriterCompleteHeader(TPM_WRITER* Writer)
{
    if (!Writer->Overflow && Writer->Capacity >= 6)
    {
        Writer->Buffer[2] = (uint8_t)(Writer->Offset >> 24);
        Writer->Buffer[3] = (uint8_t)(Writer->Offset >> 16);
        Writer->Buffer[4] = (uint8_t)(Writer->Offset >> 8);
        Writer->Buffer[5] = (uint8_t)Writer->Offset;
    }
}

static void WriterPasswordAuthArea(TPM_WRITER* Writer)
{
    WriterU32(Writer, SOFTTPM_PASSWORD_SESSION_SIZE);
    WriterU32(Writer, SOFTTPM_TPM_RS_PW);
    WriterU16(Writer, 0);
    WriterU8(Writer, 0);
    WriterU16(Writer, 0);
}

static void WriterCommandHeader(TPM_WRITER* Writer, uint16_t Tag, uint32_t CommandCode)
{
    WriterU16(Writer, Tag);
    WriterU32(Writer, 0);
    WriterU32(Writer, CommandCode);
}

static int ReaderU16(const uint8_t* Buffer, uint32_t Size, uint32_t* Offset, uint16_t* Value)
{
    if (*Offset + 2 > Size)
    {
        return -1;
    }

    *Value = (uint16_t)((Buffer[*Offset] << 8) | Buffer[*Offset + 1]);
    *Offset += 2;
    return 0;
}

uint32_t tpm_be32_get(const uint8_t* Buffer)
{
    return ((uint32_t)Buffer[0] << 24) | ((uint32_t)Buffer[1] << 16)
           | ((uint32_t)Buffer[2] << 8) | (uint32_t)Buffer[3];
}

void tpm_be16_put(uint8_t* Buffer, uint16_t Value)
{
    Buffer[0] = (uint8_t)(Value >> 8);
    Buffer[1] = (uint8_t)Value;
}

void tpm_be32_put(uint8_t* Buffer, uint32_t Value)
{
    Buffer[0] = (uint8_t)(Value >> 24);
    Buffer[1] = (uint8_t)(Value >> 16);
    Buffer[2] = (uint8_t)(Value >> 8);
    Buffer[3] = (uint8_t)Value;
}

uint32_t tpm_response_code(const uint8_t* Response, uint32_t ResponseSize)
{
    if (ResponseSize < 10)
    {
        return 0xFFFFFFFF;
    }

    return tpm_be32_get(Response + 6);
}

int tpm_response_is_success(const uint8_t* Response, uint32_t ResponseSize)
{
    return ResponseSize >= 10 && tpm_response_code(Response, ResponseSize) == SOFTTPM_TPM_RC_SUCCESS;
}

int tpm_response_parameter(
    const uint8_t* Response,
    uint32_t ResponseSize,
    const uint8_t** Parameter,
    uint32_t* ParameterSize)
{
    if (ResponseSize < 10)
    {
        return -1;
    }

    *Parameter = Response + 10;
    *ParameterSize = ResponseSize - 10;
    return 0;
}

int tpm_response_create_primary(
    const uint8_t* Response,
    uint32_t ResponseSize,
    uint32_t* ObjectHandle,
    const uint8_t** PublicBlob,
    uint32_t* PublicBlobSize)
{
    uint16_t areaSize;

    /* Session-tagged response layout: object handle, parameter size,
       public blob, remaining creation parameters. */
    if (ResponseSize < 20
        || Response[0] != (uint8_t)(SOFTTPM_TPM_ST_SESSIONS >> 8)
        || Response[1] != (uint8_t)SOFTTPM_TPM_ST_SESSIONS)
    {
        return -1;
    }

    areaSize = (uint16_t)((Response[18] << 8) | Response[19]);

    if ((uint32_t)areaSize + 20 > ResponseSize)
    {
        return -1;
    }

    *ObjectHandle = tpm_be32_get(Response + 10);
    *PublicBlob = Response + 18;
    *PublicBlobSize = (uint32_t)areaSize + 2;
    return 0;
}

uint32_t tpm_build_read_public(
    uint32_t ObjectHandle,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_NO_SESSIONS, SOFTTPM_TPM_READ_PUBLIC);
    WriterU32(&writer, ObjectHandle);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_nv_read_public(
    uint32_t NvIndex,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_NO_SESSIONS, SOFTTPM_TPM_NV_READ_PUBLIC);
    WriterU32(&writer, NvIndex);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_create_primary(
    uint32_t PrimaryHandle,
    const uint8_t* PublicArea,
    uint32_t PublicAreaSize,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_SESSIONS, SOFTTPM_TPM_CREATE_PRIMARY);
    WriterU32(&writer, PrimaryHandle);
    WriterPasswordAuthArea(&writer);
    WriterU16(&writer, 4);
    WriterU16(&writer, 0);
    WriterU16(&writer, 0);
    WriterU16(&writer, (uint16_t)PublicAreaSize);
    WriterBytes(&writer, PublicArea, PublicAreaSize);
    WriterU16(&writer, 0);
    WriterU32(&writer, 0);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_pcr_extend(
    uint32_t PcrHandle,
    const uint8_t* DigestValues,
    uint32_t DigestValuesSize,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_SESSIONS, SOFTTPM_TPM_PCR_EXTEND);
    WriterU32(&writer, PcrHandle);
    WriterPasswordAuthArea(&writer);
    WriterBytes(&writer, DigestValues, DigestValuesSize);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_evict_control(
    uint32_t AuthHandle,
    uint32_t ObjectHandle,
    uint32_t PersistentHandle,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_SESSIONS, SOFTTPM_TPM_EVICT_CONTROL);
    WriterU32(&writer, AuthHandle);
    WriterU32(&writer, ObjectHandle);
    WriterPasswordAuthArea(&writer);
    WriterU32(&writer, PersistentHandle);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_nv_define_space(
    uint32_t AuthHandle,
    uint32_t NvIndex,
    uint16_t NameAlg,
    uint32_t Attributes,
    uint16_t DataSize,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_SESSIONS, SOFTTPM_TPM_NV_DEFINE_SPACE);
    WriterU32(&writer, AuthHandle);
    WriterPasswordAuthArea(&writer);
    WriterU16(&writer, 0);
    WriterU16(&writer, 14);
    WriterU32(&writer, NvIndex);
    WriterU16(&writer, NameAlg);
    WriterU32(&writer, Attributes);
    WriterU16(&writer, 0);
    WriterU16(&writer, DataSize);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_nv_write(
    uint32_t AuthHandle,
    uint32_t NvIndex,
    uint16_t Offset,
    const uint8_t* Data,
    uint16_t DataSize,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterCommandHeader(&writer, SOFTTPM_TPM_ST_SESSIONS, SOFTTPM_TPM_NV_WRITE);
    WriterU32(&writer, AuthHandle);
    WriterU32(&writer, NvIndex);
    WriterPasswordAuthArea(&writer);
    WriterU16(&writer, DataSize);
    WriterBytes(&writer, Data, DataSize);
    WriterU16(&writer, Offset);
    WriterCompleteHeader(&writer);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_build_rsa_public_area(
    uint32_t ObjectAttributes,
    const uint8_t* AuthPolicy,
    uint16_t AuthPolicySize,
    uint8_t* Buffer,
    uint32_t BufferSize)
{
    TPM_WRITER writer;

    WriterInit(&writer, Buffer, BufferSize);
    WriterU16(&writer, SOFTTPM_TPM_ALG_RSA);
    WriterU16(&writer, SOFTTPM_TPM_ALG_SHA256);
    WriterU32(&writer, ObjectAttributes);
    WriterU16(&writer, AuthPolicySize);
    WriterBytes(&writer, AuthPolicy, AuthPolicySize);
    WriterU16(&writer, SOFTTPM_TPM_ALG_AES);
    WriterU16(&writer, 128);
    WriterU16(&writer, SOFTTPM_TPM_ALG_CFB);
    WriterU16(&writer, SOFTTPM_TPM_ALG_NULL);
    WriterU16(&writer, 2048);
    WriterU32(&writer, 0);
    WriterU16(&writer, 0);

    return writer.Overflow ? 0 : writer.Offset;
}

uint32_t tpm_public_size_of(const uint8_t* PublicArea, uint32_t AreaSize)
{
    uint16_t size;

    if (AreaSize < 2)
    {
        return 0;
    }

    size = (uint16_t)((PublicArea[0] << 8) | PublicArea[1]);
    return (uint32_t)size + 2;
}

int tpm_public_extract_rsa_modulus(
    const uint8_t* PublicArea,
    uint32_t PublicAreaSize,
    const uint8_t** Modulus,
    uint32_t* ModulusSize)
{
    uint32_t offset = 0;
    uint16_t type;
    uint16_t authPolicySize;
    uint16_t symmetricAlg;
    uint16_t scheme;
    uint16_t uniqueSize;

    if (ReaderU16(PublicArea, PublicAreaSize, &offset, &type) != 0
        || type != SOFTTPM_TPM_ALG_RSA)
    {
        return -1;
    }

    if (offset + 2 + 4 > PublicAreaSize)
    {
        return -1;
    }

    offset += 6;

    if (ReaderU16(PublicArea, PublicAreaSize, &offset, &authPolicySize) != 0
        || offset + authPolicySize > PublicAreaSize)
    {
        return -1;
    }

    offset += authPolicySize;

    if (ReaderU16(PublicArea, PublicAreaSize, &offset, &symmetricAlg) != 0)
    {
        return -1;
    }

    if (symmetricAlg != SOFTTPM_TPM_ALG_NULL)
    {
        if (offset + 4 > PublicAreaSize)
        {
            return -1;
        }

        offset += 4;
    }

    if (ReaderU16(PublicArea, PublicAreaSize, &offset, &scheme) != 0)
    {
        return -1;
    }

    if (scheme != SOFTTPM_TPM_ALG_NULL)
    {
        if (offset + 2 > PublicAreaSize)
        {
            return -1;
        }

        offset += 2;
    }

    if (offset + 2 + 4 > PublicAreaSize)
    {
        return -1;
    }

    offset += 6;

    if (ReaderU16(PublicArea, PublicAreaSize, &offset, &uniqueSize) != 0
        || uniqueSize == 0
        || offset + uniqueSize > PublicAreaSize)
    {
        return -1;
    }

    *Modulus = PublicArea + offset;
    *ModulusSize = uniqueSize;
    return 0;
}
