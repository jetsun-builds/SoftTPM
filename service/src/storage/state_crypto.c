
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <string.h>

#include "state_crypto.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#define SOFTTPM_ENTROPY_PREFIX "SoftTpm.StateKey.v1"
#define SOFTTPM_MACHINE_GUID_KEY \
    L"SOFTWARE\\Microsoft\\Cryptography"
#define SOFTTPM_MACHINE_GUID_VALUE L"MachineGuid"

typedef struct _SOFTTPM_CNG_HASH
{
    BCRYPT_ALG_HANDLE Algorithm;
    BCRYPT_HASH_HANDLE Hash;
    PUCHAR Object;
    ULONG ObjectSize;
} SOFTTPM_CNG_HASH;

static int SoftTpmSha256Internal(
    const unsigned char* Part1,
    size_t Part1Size,
    const unsigned char* Part2,
    size_t Part2Size,
    unsigned char Digest[SOFTTPM_STATE_KEY_SIZE])
{
    SOFTTPM_CNG_HASH hash;
    NTSTATUS status;
    ULONG result = 0;
    unsigned char digest[SOFTTPM_STATE_KEY_SIZE];

    memset(&hash, 0, sizeof(hash));

    status = BCryptOpenAlgorithmProvider(
        &hash.Algorithm,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        0);

    if (!NT_SUCCESS(status))
    {
        return -1;
    }

    status = BCryptGetProperty(
        hash.Algorithm,
        BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&hash.ObjectSize,
        sizeof(hash.ObjectSize),
        &result,
        0);

    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hash.Algorithm, 0);
        return -1;
    }

    hash.Object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, hash.ObjectSize);
    if (hash.Object == NULL)
    {
        BCryptCloseAlgorithmProvider(hash.Algorithm, 0);
        return -1;
    }

    status = BCryptCreateHash(
        hash.Algorithm,
        &hash.Hash,
        hash.Object,
        hash.ObjectSize,
        NULL,
        0,
        0);

    if (!NT_SUCCESS(status))
    {
        HeapFree(GetProcessHeap(), 0, hash.Object);
        BCryptCloseAlgorithmProvider(hash.Algorithm, 0);
        return -1;
    }

    if (Part1Size > 0)
    {
        status = BCryptHashData(
            hash.Hash,
            (PUCHAR)Part1,
            (ULONG)Part1Size,
            0);
    }

    if (NT_SUCCESS(status) && Part2Size > 0)
    {
        status = BCryptHashData(
            hash.Hash,
            (PUCHAR)Part2,
            (ULONG)Part2Size,
            0);
    }

    if (NT_SUCCESS(status))
    {
        status = BCryptFinishHash(
            hash.Hash,
            digest,
            (ULONG)sizeof(digest),
            0);
    }

    BCryptDestroyHash(hash.Hash);
    HeapFree(GetProcessHeap(), 0, hash.Object);
    BCryptCloseAlgorithmProvider(hash.Algorithm, 0);

    if (!NT_SUCCESS(status))
    {
        return -1;
    }

    memcpy(Digest, digest, sizeof(digest));
    return 0;
}

int softtpm_random(unsigned char* Buffer, size_t Size)
{
    NTSTATUS status;

    if (Buffer == NULL || Size == 0)
    {
        return -1;
    }

    status = BCryptGenRandom(
        NULL,
        Buffer,
        (ULONG)Size,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    return NT_SUCCESS(status) ? 0 : -1;
}

int softtpm_sha256(
    const unsigned char* Data,
    size_t DataSize,
    unsigned char Digest[SOFTTPM_STATE_KEY_SIZE])
{
    return SoftTpmSha256Internal(Data, DataSize, NULL, 0, Digest);
}

int softtpm_machine_entropy(unsigned char Digest[SOFTTPM_STATE_KEY_SIZE])
{
    WCHAR machineGuid[256];
    DWORD machineGuidSize = sizeof(machineGuid);
    LSTATUS status;

    machineGuid[0] = 0;

    status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        SOFTTPM_MACHINE_GUID_KEY,
        SOFTTPM_MACHINE_GUID_VALUE,
        RRF_RT_REG_SZ,
        NULL,
        machineGuid,
        &machineGuidSize);

    if (status != ERROR_SUCCESS)
    {
        machineGuid[0] = 0;
        machineGuidSize = 0;
    }

    return SoftTpmSha256Internal(
        (const unsigned char*)SOFTTPM_ENTROPY_PREFIX,
        sizeof(SOFTTPM_ENTROPY_PREFIX) - 1,
        (const unsigned char*)machineGuid,
        machineGuidSize == 0 ? 0 : wcslen(machineGuid) * sizeof(WCHAR),
        Digest);
}

int softtpm_protect_key(
    const unsigned char* Key,
    size_t KeySize,
    unsigned char** Blob,
    size_t* BlobSize)
{
    DATA_BLOB input;
    DATA_BLOB entropy;
    DATA_BLOB output;
    unsigned char entropyDigest[SOFTTPM_STATE_KEY_SIZE];

    if (Key == NULL || KeySize > MAXDWORD || Blob == NULL || BlobSize == NULL)
    {
        return -1;
    }

    if (softtpm_machine_entropy(entropyDigest) != 0)
    {
        return -1;
    }

    input.pbData = (BYTE*)Key;
    input.cbData = (DWORD)KeySize;
    entropy.pbData = entropyDigest;
    entropy.cbData = sizeof(entropyDigest);
    output.pbData = NULL;
    output.cbData = 0;

    if (!CryptProtectData(
            &input,
            L"SoftTpm TPM state key",
            &entropy,
            NULL,
            NULL,
            CRYPTPROTECT_LOCAL_MACHINE,
            &output))
    {
        return -1;
    }

    *Blob = (unsigned char*)LocalAlloc(LMEM_FIXED, output.cbData);
    if (*Blob == NULL)
    {
        LocalFree(output.pbData);
        return -1;
    }

    memcpy(*Blob, output.pbData, output.cbData);
    *BlobSize = output.cbData;
    LocalFree(output.pbData);

    return 0;
}

int softtpm_unprotect_key(
    const unsigned char* Blob,
    size_t BlobSize,
    unsigned char* Key,
    size_t KeySize)
{
    DATA_BLOB input;
    DATA_BLOB entropy;
    DATA_BLOB output;
    unsigned char entropyDigest[SOFTTPM_STATE_KEY_SIZE];

    if (Blob == NULL || BlobSize > MAXDWORD || Key == NULL)
    {
        return -1;
    }

    if (softtpm_machine_entropy(entropyDigest) != 0)
    {
        return -1;
    }

    input.pbData = (BYTE*)Blob;
    input.cbData = (DWORD)BlobSize;
    entropy.pbData = entropyDigest;
    entropy.cbData = sizeof(entropyDigest);
    output.pbData = NULL;
    output.cbData = 0;

    if (!CryptUnprotectData(
            &input,
            NULL,
            &entropy,
            NULL,
            NULL,
            CRYPTPROTECT_LOCAL_MACHINE,
            &output))
    {
        return -1;
    }

    if (output.cbData != KeySize)
    {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return -1;
    }

    memcpy(Key, output.pbData, KeySize);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);

    return 0;
}

static int SoftTpmAesGcm(
    const unsigned char Key[SOFTTPM_STATE_KEY_SIZE],
    const unsigned char Nonce[SOFTTPM_STATE_NONCE_SIZE],
    const unsigned char* Aad,
    size_t AadSize,
    const unsigned char* Input,
    size_t InputSize,
    unsigned char* Output,
    unsigned char Tag[SOFTTPM_STATE_TAG_SIZE],
    int Encrypt)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    NTSTATUS status;
    ULONG written = 0;
    int result = -1;

    status = BCryptOpenAlgorithmProvider(
        &algorithm,
        BCRYPT_AES_ALGORITHM,
        NULL,
        0);

    if (!NT_SUCCESS(status))
    {
        return -1;
    }

    status = BCryptSetProperty(
        algorithm,
        BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
        sizeof(BCRYPT_CHAIN_MODE_GCM),
        0);

    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return -1;
    }

    status = BCryptGenerateSymmetricKey(
        algorithm,
        &key,
        NULL,
        0,
        (PUCHAR)Key,
        SOFTTPM_STATE_KEY_SIZE,
        0);

    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return -1;
    }

    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = (PUCHAR)Nonce;
    authInfo.cbNonce = SOFTTPM_STATE_NONCE_SIZE;
    authInfo.pbAuthData = (PUCHAR)Aad;
    authInfo.cbAuthData = (ULONG)AadSize;
    authInfo.pbTag = Tag;
    authInfo.cbTag = SOFTTPM_STATE_TAG_SIZE;

    if (Encrypt)
    {
        status = BCryptEncrypt(
            key,
            (PUCHAR)Input,
            (ULONG)InputSize,
            &authInfo,
            NULL,
            0,
            Output,
            (ULONG)InputSize,
            &written,
            0);
    }
    else
    {
        status = BCryptDecrypt(
            key,
            (PUCHAR)Input,
            (ULONG)InputSize,
            &authInfo,
            NULL,
            0,
            Output,
            (ULONG)InputSize,
            &written,
            0);
    }

    if (NT_SUCCESS(status) && written == InputSize)
    {
        result = 0;
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    return result;
}

int softtpm_aes_gcm_encrypt(
    const unsigned char Key[SOFTTPM_STATE_KEY_SIZE],
    const unsigned char Nonce[SOFTTPM_STATE_NONCE_SIZE],
    const unsigned char* Aad,
    size_t AadSize,
    const unsigned char* Plain,
    size_t PlainSize,
    unsigned char* Cipher,
    unsigned char Tag[SOFTTPM_STATE_TAG_SIZE])
{
    return SoftTpmAesGcm(
        Key,
        Nonce,
        Aad,
        AadSize,
        Plain,
        PlainSize,
        Cipher,
        Tag,
        1);
}

int softtpm_aes_gcm_decrypt(
    const unsigned char Key[SOFTTPM_STATE_KEY_SIZE],
    const unsigned char Nonce[SOFTTPM_STATE_NONCE_SIZE],
    const unsigned char* Aad,
    size_t AadSize,
    const unsigned char* Cipher,
    size_t CipherSize,
    const unsigned char Tag[SOFTTPM_STATE_TAG_SIZE],
    unsigned char* Plain)
{
    return SoftTpmAesGcm(
        Key,
        Nonce,
        Aad,
        AadSize,
        Cipher,
        CipherSize,
        Plain,
        (unsigned char*)Tag,
        0);
}
