#include "platform_internal.h"

static const PCR_Attributes s_initAttributes[] = {
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {1, 0, 0, 0, 0, 0x1F},
    {0, 0, 0, 0, 0x0F, 0x1F},
    {0, 0, 0, 0, 0x10, 0x1C},
    {0, 0, 0, 0, 0x10, 0x1C},
    {0, 0, 0, 0, 0x10, 0x0C},
    {0, 1, 1, 1, 0x14, 0x0E},
    {0, 1, 1, 1, 0x14, 0x04},
    {0, 1, 1, 1, 0x14, 0x04},
    {0, 0, 0, 0, 0x0F, 0x1F},
};

#define SOFTTPM_PCR_ATTRIBUTE_COUNT \
    (sizeof(s_initAttributes) / sizeof(s_initAttributes[0]))

MUST_BE(SOFTTPM_PCR_ATTRIBUTE_COUNT == IMPLEMENTATION_PCR);

#if ALG_SHA256 != YES && ALG_SHA384 != YES
#  error No default PCR banks defined
#endif

static const TPM_ALG_ID DefaultActivePcrBanks[] = {
#if ALG_SHA256
    TPM_ALG_SHA256
#endif
#if ALG_SHA384
#  if ALG_SHA256
    ,
#  endif
    TPM_ALG_SHA384
#endif
#if ALG_SHA512
#  if ALG_SHA256 || ALG_SHA384
    ,
#  endif
    TPM_ALG_SHA512
#endif
};

static uint16_t SoftTpmHashDigestSize(TPM_ALG_ID hashAlg)
{
    switch (hashAlg)
    {
#if ALG_SHA1
    case TPM_ALG_SHA1:
        return SHA1_DIGEST_SIZE;
#endif
#if ALG_SHA256
    case TPM_ALG_SHA256:
        return SHA256_DIGEST_SIZE;
#endif
#if ALG_SHA384
    case TPM_ALG_SHA384:
        return SHA384_DIGEST_SIZE;
#endif
#if ALG_SHA512
    case TPM_ALG_SHA512:
        return SHA512_DIGEST_SIZE;
#endif
    default:
        return 0;
    }
}

UINT32 _platPcr__NumberOfPcrs(void)
{
    return (UINT32)SOFTTPM_PCR_ATTRIBUTE_COUNT;
}

PCR_Attributes _platPcr__GetPcrInitializationAttributes(UINT32 pcrNumber)
{
    if (pcrNumber >= _platPcr__NumberOfPcrs())
    {
        pcrNumber = 0;
    }

    return s_initAttributes[pcrNumber];
}

BOOL _platPcr_IsPcrBankDefaultActive(TPM_ALG_ID pcrAlg)
{
    int index;

    for (index = 0; index < (int)(sizeof(DefaultActivePcrBanks) / sizeof(DefaultActivePcrBanks[0])); index++)
    {
        if (DefaultActivePcrBanks[index] == pcrAlg)
        {
            return TRUE;
        }
    }

    return FALSE;
}

TPM_RC _platPcr__GetInitialValueForPcr(
    UINT32 pcrNumber,
    TPM_ALG_ID pcrAlg,
    BYTE startupLocality,
    BYTE* pcrData,
    uint16_t bufferSize,
    uint16_t* pcrLength)
{
    PCR_Attributes pcrAttributes;
    uint16_t pcrSize;
    BYTE defaultValue = 0;

    pcrSize = SoftTpmHashDigestSize(pcrAlg);

    if (pcrSize == 0 || pcrData == NULL || pcrLength == NULL)
    {
        return TPM_RC_FAILURE;
    }

    if (pcrNumber >= _platPcr__NumberOfPcrs() || bufferSize < pcrSize)
    {
        return TPM_RC_FAILURE;
    }

    pcrAttributes = _platPcr__GetPcrInitializationAttributes(pcrNumber);

    if ((pcrAttributes.resetLocality & 0x10) != 0)
    {
        defaultValue = 0xFF;
    }

    memset(pcrData, defaultValue, pcrSize);

    if (pcrNumber == HCRTM_PCR)
    {
        pcrData[pcrSize - 1] = startupLocality;
    }

    *pcrLength = pcrSize;
    return TPM_RC_SUCCESS;
}
