
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "platform_log.h"
#include "log.h"
#include "engine.h"
#include "provision/tpm_commands.h"

#define SOFTTPM_LOG_MAX_SIZE (2u * 1024u * 1024u)
#define SOFTTPM_LOG_MIN_SIZE 65u

#define SOFTTPM_LOG_EVENT_NO_ACTION 0x00000003u
#define SOFTTPM_LOG_HEADER_DIGEST_SIZE 20u
#define SOFTTPM_LOG_HEADER_STRUCT_SIZE 28u
#define SOFTTPM_LOG_V1_EVENT_SIZE 32u
#define SOFTTPM_LOG_MAX_ALGORITHMS 16u
#define SOFTTPM_LOG_MAX_DIGESTS 8u
#define SOFTTPM_LOG_MAX_PCR 23u
#define SOFTTPM_LOG_FIRST_DRTM_PCR 17u
#define SOFTTPM_LOG_LAST_DRTM_PCR 22u

#define SOFTTPM_LOG_EXTEND_COMMAND_SIZE 1024u
#define SOFTTPM_LOG_EXTEND_RESPONSE_SIZE 64u
#define SOFTTPM_LOG_DIGEST_VALUES_SIZE (4u + SOFTTPM_LOG_MAX_DIGESTS * (2u + 64u))

#define SOFTTPM_LOG_ALG_SHA256 0x000Bu
#define SOFTTPM_LOG_ALG_SHA384 0x000Cu
#define SOFTTPM_LOG_ALG_SHA512 0x000Du

#define SOFTTPM_LOG_SPEC_ID_SIGNATURE "Spec ID Event03"

typedef struct
{
    uint16_t Alg;
    uint16_t Size;
} SOFTTPM_LOG_ALG;

typedef struct
{
    uint32_t PcrIndex;
    uint32_t EventType;
    uint32_t DigestCount;
    const unsigned char* DigestValues;
    uint32_t DigestValuesSize;
    const unsigned char* EventData;
    uint32_t EventDataSize;
} SOFTTPM_LOG_EVENT;

static const SOFTTPM_LOG_ALG s_supportedAlgorithms[] = {
    {SOFTTPM_LOG_ALG_SHA256, 32},
    {SOFTTPM_LOG_ALG_SHA384, 48},
    {SOFTTPM_LOG_ALG_SHA512, 64},
};

static SOFTTPM_LOG_ALG s_algorithms[SOFTTPM_LOG_MAX_ALGORITHMS];
static uint32_t s_algorithmCount = 0;

static unsigned char* s_logData = NULL;
static uint32_t s_logSize = 0;

static uint16_t SoftTpmLogReadU16(const unsigned char* Buffer)
{
    return (uint16_t)(((uint16_t)Buffer[1] << 8) | Buffer[0]);
}

static uint32_t SoftTpmLogReadU32(const unsigned char* Buffer)
{
    return ((uint32_t)Buffer[3] << 24) | ((uint32_t)Buffer[2] << 16)
           | ((uint32_t)Buffer[1] << 8) | (uint32_t)Buffer[0];
}

static int SoftTpmLogFindAlgorithm(uint16_t Alg, uint16_t* Size)
{
    uint32_t index;

    for (index = 0; index < s_algorithmCount; index++)
    {
        if (s_algorithms[index].Alg == Alg)
        {
            *Size = s_algorithms[index].Size;
            return 0;
        }
    }

    return -1;
}

static int SoftTpmLogValidateHeader(
    const unsigned char* Data,
    uint32_t Size,
    uint32_t* Offset)
{
    const unsigned char* event;
    uint32_t eventSize;
    uint32_t algorithmCount;
    uint32_t index;
    unsigned char vendorInfoSize;

    if (Size < SOFTTPM_LOG_MIN_SIZE)
    {
        log_warn("platform log: too small for a TCG log header (%u bytes)", Size);
        return -1;
    }

    if (SoftTpmLogReadU32(Data) != 0
        || SoftTpmLogReadU32(Data + 4) != SOFTTPM_LOG_EVENT_NO_ACTION)
    {
        log_warn("platform log: the first event is not a PCR 0 EV_NO_ACTION header");
        return -1;
    }

    for (index = 0; index < SOFTTPM_LOG_HEADER_DIGEST_SIZE; index++)
    {
        if (Data[8 + index] != 0)
        {
            log_warn("platform log: the header digest is not zero");
            return -1;
        }
    }

    eventSize = SoftTpmLogReadU32(Data + 28);

    if (eventSize < SOFTTPM_LOG_HEADER_STRUCT_SIZE + 1u
        || eventSize > Size - SOFTTPM_LOG_V1_EVENT_SIZE)
    {
        log_warn("platform log: invalid spec ID event size (%u)", eventSize);
        return -1;
    }

    event = Data + SOFTTPM_LOG_V1_EVENT_SIZE;

    if (memcmp(event, SOFTTPM_LOG_SPEC_ID_SIGNATURE, sizeof(SOFTTPM_LOG_SPEC_ID_SIGNATURE)) != 0)
    {
        log_warn("platform log: unsupported header signature (expected \"%s\")", SOFTTPM_LOG_SPEC_ID_SIGNATURE);
        return -1;
    }

    algorithmCount = SoftTpmLogReadU32(event + 24);

    if (algorithmCount < 1 || algorithmCount > SOFTTPM_LOG_MAX_ALGORITHMS)
    {
        log_warn("platform log: invalid algorithm count (%u)", algorithmCount);
        return -1;
    }

    if (SOFTTPM_LOG_HEADER_STRUCT_SIZE + 4u * algorithmCount + 1u > eventSize)
    {
        log_warn("platform log: the algorithm table extends past the spec ID event");
        return -1;
    }

    vendorInfoSize = event[SOFTTPM_LOG_HEADER_STRUCT_SIZE + 4u * algorithmCount];

    if (SOFTTPM_LOG_HEADER_STRUCT_SIZE + 4u * algorithmCount + 1u + vendorInfoSize != eventSize)
    {
        log_warn("platform log: the spec ID event size does not match its contents");
        return -1;
    }

    s_algorithmCount = 0;

    for (index = 0; index < algorithmCount; index++)
    {
        uint16_t alg = SoftTpmLogReadU16(event + SOFTTPM_LOG_HEADER_STRUCT_SIZE + 4u * index);
        uint16_t algSize = SoftTpmLogReadU16(event + SOFTTPM_LOG_HEADER_STRUCT_SIZE + 4u * index + 2u);
        uint32_t supportedIndex;
        int found = 0;

        for (supportedIndex = 0;
             supportedIndex < sizeof(s_supportedAlgorithms) / sizeof(s_supportedAlgorithms[0]);
             supportedIndex++)
        {
            if (s_supportedAlgorithms[supportedIndex].Alg == alg)
            {
                if (s_supportedAlgorithms[supportedIndex].Size != algSize)
                {
                    log_warn("platform log: algorithm 0x%04X reports digest size %u", alg, algSize);
                    return -1;
                }

                found = 1;
                break;
            }
        }

        if (!found)
        {
            log_warn("platform log: unsupported digest algorithm 0x%04X", alg);
            return -1;
        }

        s_algorithms[index].Alg = alg;
        s_algorithms[index].Size = algSize;
    }

    s_algorithmCount = algorithmCount;
    *Offset = SOFTTPM_LOG_V1_EVENT_SIZE + eventSize;

    return 0;
}

static int SoftTpmLogParseEvent(
    const unsigned char* Data,
    uint32_t Size,
    uint32_t* Offset,
    SOFTTPM_LOG_EVENT* Event)
{
    uint32_t offset = *Offset;
    uint32_t valuesStart;
    uint32_t index;

    if (offset + 16u > Size)
    {
        log_warn("platform log: truncated event at offset %u", offset);
        return -1;
    }

    Event->PcrIndex = SoftTpmLogReadU32(Data + offset);
    Event->EventType = SoftTpmLogReadU32(Data + offset + 4);
    Event->DigestCount = SoftTpmLogReadU32(Data + offset + 8);

    if (Event->DigestCount > SOFTTPM_LOG_MAX_DIGESTS)
    {
        log_warn("platform log: event at offset %u lists %u digests", offset, Event->DigestCount);
        return -1;
    }

    valuesStart = offset + 8;
    offset += 12;

    for (index = 0; index < Event->DigestCount; index++)
    {
        uint16_t alg;
        uint16_t digestSize;

        if (offset + 2u > Size)
        {
            log_warn("platform log: truncated digest list at offset %u", offset);
            return -1;
        }

        alg = SoftTpmLogReadU16(Data + offset);
        offset += 2;

        if (SoftTpmLogFindAlgorithm(alg, &digestSize) != 0)
        {
            log_warn("platform log: digest algorithm 0x%04X is not in the spec ID event", alg);
            return -1;
        }

        if (offset + digestSize > Size)
        {
            log_warn("platform log: truncated digest at offset %u", offset);
            return -1;
        }

        offset += digestSize;
    }

    Event->DigestValues = Data + valuesStart;
    Event->DigestValuesSize = offset - valuesStart;

    if (offset + 4u > Size)
    {
        log_warn("platform log: truncated event data size at offset %u", offset);
        return -1;
    }

    Event->EventDataSize = SoftTpmLogReadU32(Data + offset);
    offset += 4;

    if (Event->EventDataSize > Size - offset)
    {
        log_warn("platform log: truncated event data at offset %u", offset);
        return -1;
    }

    Event->EventData = Data + offset;
    offset += Event->EventDataSize;

    *Offset = offset;

    return 0;
}

static int SoftTpmLogValidate(const unsigned char* Data, uint32_t Size)
{
    uint32_t offset = 0;
    SOFTTPM_LOG_EVENT event;

    if (SoftTpmLogValidateHeader(Data, Size, &offset) != 0)
    {
        return -1;
    }

    while (offset < Size)
    {
        if (SoftTpmLogParseEvent(Data, Size, &offset, &event) != 0)
        {
            return -1;
        }

        if (event.PcrIndex > SOFTTPM_LOG_MAX_PCR)
        {
            log_warn("platform log: invalid PCR index %u", event.PcrIndex);
            return -1;
        }

        if (event.PcrIndex >= SOFTTPM_LOG_FIRST_DRTM_PCR
            && event.PcrIndex <= SOFTTPM_LOG_LAST_DRTM_PCR)
        {
            log_warn("platform log: PCR %u cannot be replayed from locality 0", event.PcrIndex);
            return -1;
        }
    }

    return 0;
}

static uint32_t SoftTpmLogBuildDigestValues(
    const SOFTTPM_LOG_EVENT* Event,
    unsigned char* Buffer,
    uint32_t BufferSize)
{
    uint32_t source = 4;
    uint32_t written = 0;
    uint32_t index;
    uint32_t count = Event->DigestCount;

    if (Event->DigestValuesSize < 4u
        || SoftTpmLogReadU32(Event->DigestValues) != count)
    {
        return 0;
    }

    if (written + 4u > BufferSize)
    {
        return 0;
    }

    Buffer[written++] = (unsigned char)((count >> 24) & 0xFF);
    Buffer[written++] = (unsigned char)((count >> 16) & 0xFF);
    Buffer[written++] = (unsigned char)((count >> 8) & 0xFF);
    Buffer[written++] = (unsigned char)(count & 0xFF);

    for (index = 0; index < count; index++)
    {
        uint16_t alg;
        uint16_t digestSize;

        if (source + 2u > Event->DigestValuesSize)
        {
            return 0;
        }

        alg = SoftTpmLogReadU16(Event->DigestValues + source);
        source += 2;

        if (SoftTpmLogFindAlgorithm(alg, &digestSize) != 0)
        {
            return 0;
        }

        if (source + digestSize > Event->DigestValuesSize
            || written + 2u + digestSize > BufferSize)
        {
            return 0;
        }

        Buffer[written++] = (unsigned char)((alg >> 8) & 0xFF);
        Buffer[written++] = (unsigned char)(alg & 0xFF);
        memcpy(Buffer + written, Event->DigestValues + source, digestSize);
        written += digestSize;
        source += digestSize;
    }

    return written;
}

static int SoftTpmLogReplay(const unsigned char* Data, uint32_t Size)
{
    uint32_t offset = SOFTTPM_LOG_V1_EVENT_SIZE + SoftTpmLogReadU32(Data + 28);
    uint32_t eventIndex = 0;
    uint32_t extendedEvents = 0;
    SOFTTPM_LOG_EVENT event;

    while (offset < Size)
    {
        unsigned char command[SOFTTPM_LOG_EXTEND_COMMAND_SIZE];
        unsigned char response[SOFTTPM_LOG_EXTEND_RESPONSE_SIZE];
        unsigned char digestValues[SOFTTPM_LOG_DIGEST_VALUES_SIZE];
        uint32_t commandSize;
        uint32_t digestValuesSize;
        uint32_t responseSize = sizeof(response);

        if (SoftTpmLogParseEvent(Data, Size, &offset, &event) != 0)
        {
            log_error("platform log: replay stopped while parsing event %u", eventIndex);
            return -1;
        }

        eventIndex++;

        if (event.EventType == SOFTTPM_LOG_EVENT_NO_ACTION || event.DigestCount == 0)
        {
            continue;
        }

        digestValuesSize = SoftTpmLogBuildDigestValues(
            &event,
            digestValues,
            sizeof(digestValues));

        if (digestValuesSize == 0)
        {
            log_error("platform log: cannot convert the PCR %u digest list for the extend command", event.PcrIndex);
            return -1;
        }

        commandSize = tpm_build_pcr_extend(
            event.PcrIndex,
            digestValues,
            digestValuesSize,
            command,
            sizeof(command));

        if (commandSize == 0)
        {
            log_error("platform log: cannot build the PCR %u extend command", event.PcrIndex);
            return -1;
        }

        if (engine_execute(command, commandSize, response, &responseSize) != 0)
        {
            log_error("platform log: the engine rejected the PCR %u extend command", event.PcrIndex);
            return -1;
        }

        if (!tpm_response_is_success(response, responseSize))
        {
            log_error(
                "platform log: PCR %u extend failed with 0x%08X",
                event.PcrIndex,
                tpm_response_code(response, responseSize));
            return -1;
        }

        extendedEvents++;

        log_debug(
            "platform log: extended PCR %u with %u digest(s) (event %u)",
            event.PcrIndex,
            event.DigestCount,
            eventIndex);
    }

    log_info(
        "platform log: replayed %u measurement event(s) out of %u",
        extendedEvents,
        eventIndex);

    return 0;
}

int platform_log_load_and_replay(const wchar_t* Path)
{
    HANDLE file;
    LARGE_INTEGER fileSize;
    unsigned char* data;
    uint32_t size;
    uint32_t totalRead = 0;

    if (Path == NULL || Path[0] == L'\0')
    {
        return -1;
    }

    file = CreateFileW(
        Path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        log_info("platform log: no log at %ls, publishing the synthetic log", Path);
        return -1;
    }

    if (!GetFileSizeEx(file, &fileSize)
        || fileSize.QuadPart <= 0
        || fileSize.QuadPart > (LONGLONG)SOFTTPM_LOG_MAX_SIZE)
    {
        log_warn("platform log: %ls has an unsupported size", Path);
        CloseHandle(file);
        return -1;
    }

    size = (uint32_t)fileSize.QuadPart;
    data = malloc(size);

    if (data == NULL)
    {
        log_warn("platform log: cannot allocate %u bytes", size);
        CloseHandle(file);
        return -1;
    }

    while (totalRead < size)
    {
        DWORD readNow = 0;

        if (!ReadFile(file, data + totalRead, size - totalRead, &readNow, NULL)
            || readNow == 0)
        {
            log_warn("platform log: cannot read %ls", Path);
            free(data);
            CloseHandle(file);
            return -1;
        }

        totalRead += readNow;
    }

    CloseHandle(file);

    if (SoftTpmLogValidate(data, size) != 0)
    {
        log_warn("platform log: ignoring %ls (not a supported TCG event log)", Path);
        free(data);
        return -1;
    }

    log_info(
        "platform log: loaded %ls (%u bytes, %u digest algorithm(s))",
        Path,
        size,
        s_algorithmCount);

    if (SoftTpmLogReplay(data, size) != 0)
    {
        log_warn("platform log: replay failed, publishing the synthetic log instead");
        free(data);
        return -1;
    }

    s_logData = data;
    s_logSize = size;

    return 0;
}

const unsigned char* platform_log_data(uint32_t* Size)
{
    if (s_logData == NULL)
    {
        if (Size != NULL)
        {
            *Size = 0;
        }

        return NULL;
    }

    if (Size != NULL)
    {
        *Size = s_logSize;
    }

    return s_logData;
}
