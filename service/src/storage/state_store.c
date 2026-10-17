
#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "state_store.h"
#include "log.h"

#define SOFTTPM_STATE_MAGIC "SFTSTATE"
#define SOFTTPM_STATE_MAGIC_SIZE 8
#define SOFTTPM_STATE_VERSION 1
#define SOFTTPM_STATE_FLAG_MANUFACTURED 0x0001
#define SOFTTPM_STATE_HEADER_SIZE 48
#define SOFTTPM_STATE_AAD_SIZE 32

#define SOFTTPM_KEY_MAGIC "SFTKEY01"
#define SOFTTPM_KEY_MAGIC_SIZE 8
#define SOFTTPM_KEY_VERSION 1
#define SOFTTPM_KEY_HEADER_SIZE 16

static void WriteLe16(unsigned char* Buffer, unsigned short Value)
{
    Buffer[0] = (unsigned char)(Value & 0xFF);
    Buffer[1] = (unsigned char)((Value >> 8) & 0xFF);
}

static void WriteLe32(unsigned char* Buffer, unsigned int Value)
{
    Buffer[0] = (unsigned char)(Value & 0xFF);
    Buffer[1] = (unsigned char)((Value >> 8) & 0xFF);
    Buffer[2] = (unsigned char)((Value >> 16) & 0xFF);
    Buffer[3] = (unsigned char)((Value >> 24) & 0xFF);
}

static unsigned short ReadLe16(const unsigned char* Buffer)
{
    return (unsigned short)(Buffer[0] | (Buffer[1] << 8));
}

static unsigned int ReadLe32(const unsigned char* Buffer)
{
    return (unsigned int)Buffer[0]
        | ((unsigned int)Buffer[1] << 8)
        | ((unsigned int)Buffer[2] << 16)
        | ((unsigned int)Buffer[3] << 24);
}

static int ReadWholeFile(const wchar_t* Path, unsigned char* Buffer, size_t BufferSize, size_t* BytesRead)
{
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read = 0;
    BOOL ok;

    file = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 1 : -1;
    }

    if (!GetFileSizeEx(file, &size) || size.QuadPart > (LONGLONG)BufferSize)
    {
        CloseHandle(file);
        return -1;
    }

    ok = ReadFile(file, Buffer, (DWORD)size.QuadPart, &read, NULL);
    CloseHandle(file);

    if (!ok || read != (DWORD)size.QuadPart)
    {
        return -1;
    }

    *BytesRead = read;
    return 0;
}

static int WriteFileAtomic(const wchar_t* Path, const unsigned char* Buffer, size_t BufferSize)
{
    wchar_t tempPath[SOFTTPM_STATE_MAX_PATH + 8];
    HANDLE file;
    DWORD written = 0;
    BOOL ok;

    _snwprintf_s(
        tempPath,
        sizeof(tempPath) / sizeof(wchar_t),
        _TRUNCATE,
        L"%ls.tmp",
        Path);

    file = CreateFileW(
        tempPath,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        log_error("state: cannot create %ls (error %lu)", tempPath, GetLastError());
        return -1;
    }

    ok = WriteFile(file, Buffer, (DWORD)BufferSize, &written, NULL);
    ok = ok && written == (DWORD)BufferSize;
    ok = ok && FlushFileBuffers(file);
    CloseHandle(file);

    if (!ok)
    {
        log_error("state: cannot write %ls (error %lu)", tempPath, GetLastError());
        DeleteFileW(tempPath);
        return -1;
    }

    if (!MoveFileExW(
            tempPath,
            Path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        log_error("state: cannot replace %ls (error %lu)", Path, GetLastError());
        DeleteFileW(tempPath);
        return -1;
    }

    return 0;
}

static int EnsureDirectory(const wchar_t* Directory)
{
    DWORD attributes = GetFileAttributesW(Directory);

    if (attributes != INVALID_FILE_ATTRIBUTES)
    {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 0 : -1;
    }

    if (CreateDirectoryW(Directory, NULL))
    {
        return 0;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    return -1;
}

static int LoadOrCreateKey(SOFTTPM_STATE_STORE* Store)
{
    unsigned char fileBuffer[4096];
    size_t fileSize = 0;
    int status;

    status = ReadWholeFile(Store->KeyPath, fileBuffer, sizeof(fileBuffer), &fileSize);

    if (status == 0)
    {
        size_t blobSize;

        if (fileSize < SOFTTPM_KEY_HEADER_SIZE
            || memcmp(fileBuffer, SOFTTPM_KEY_MAGIC, SOFTTPM_KEY_MAGIC_SIZE) != 0
            || ReadLe16(fileBuffer + 8) != SOFTTPM_KEY_VERSION)
        {
            log_error("state: key file %ls is not valid", Store->KeyPath);
            return -1;
        }

        blobSize = ReadLe32(fileBuffer + 12);

        if (blobSize != fileSize - SOFTTPM_KEY_HEADER_SIZE)
        {
            log_error("state: key file %ls is truncated", Store->KeyPath);
            return -1;
        }

        if (softtpm_unprotect_key(
                fileBuffer + SOFTTPM_KEY_HEADER_SIZE,
                blobSize,
                Store->Key,
                sizeof(Store->Key)) != 0)
        {
            log_error("state: cannot unprotect the state key; refusing to continue");
            return -1;
        }

        Store->KeyLoaded = 1;
        return 0;
    }

    if (status < 0)
    {
        log_error("state: cannot read key file %ls", Store->KeyPath);
        return -1;
    }

    if (softtpm_random(Store->Key, sizeof(Store->Key)) != 0)
    {
        log_error("state: cannot generate a state key");
        return -1;
    }

    {
        unsigned char* blob = NULL;
        size_t blobSize = 0;
        unsigned char fileHeader[SOFTTPM_KEY_HEADER_SIZE];

        if (softtpm_protect_key(Store->Key, sizeof(Store->Key), &blob, &blobSize) != 0)
        {
            log_error("state: cannot protect the state key");
            return -1;
        }

        memcpy(fileHeader, SOFTTPM_KEY_MAGIC, SOFTTPM_KEY_MAGIC_SIZE);
        WriteLe16(fileHeader + 8, SOFTTPM_KEY_VERSION);
        WriteLe16(fileHeader + 10, 0);
        WriteLe32(fileHeader + 12, (unsigned int)blobSize);

        {
            unsigned char* fileData = (unsigned char*)HeapAlloc(
                GetProcessHeap(),
                0,
                SOFTTPM_KEY_HEADER_SIZE + blobSize);

            if (fileData == NULL)
            {
                LocalFree(blob);
                return -1;
            }

            memcpy(fileData, fileHeader, SOFTTPM_KEY_HEADER_SIZE);
            memcpy(fileData + SOFTTPM_KEY_HEADER_SIZE, blob, blobSize);

            status = WriteFileAtomic(Store->KeyPath, fileData, SOFTTPM_KEY_HEADER_SIZE + blobSize);

            SecureZeroMemory(fileData, SOFTTPM_KEY_HEADER_SIZE + blobSize);
            HeapFree(GetProcessHeap(), 0, fileData);
            LocalFree(blob);

            if (status != 0)
            {
                return -1;
            }
        }
    }

    Store->KeyLoaded = 1;
    log_info("state: created new state key at %ls", Store->KeyPath);
    return 0;
}

int state_store_open(SOFTTPM_STATE_STORE* Store, const wchar_t* Directory)
{
    if (Store == NULL || Directory == NULL)
    {
        return -1;
    }

    memset(Store, 0, sizeof(*Store));

    _snwprintf_s(
        Store->Directory,
        sizeof(Store->Directory) / sizeof(wchar_t),
        _TRUNCATE,
        L"%ls",
        Directory);

    _snwprintf_s(
        Store->KeyPath,
        sizeof(Store->KeyPath) / sizeof(wchar_t),
        _TRUNCATE,
        L"%ls\\state.key",
        Directory);

    _snwprintf_s(
        Store->StatePath,
        sizeof(Store->StatePath) / sizeof(wchar_t),
        _TRUNCATE,
        L"%ls\\tpm.state",
        Directory);

    if (EnsureDirectory(Store->Directory) != 0)
    {
        log_error("state: cannot create directory %ls (error %lu)", Directory, GetLastError());
        return -1;
    }

    return LoadOrCreateKey(Store);
}

const wchar_t* state_store_directory(const SOFTTPM_STATE_STORE* Store)
{
    return Store->Directory;
}

int state_store_load_nv(
    SOFTTPM_STATE_STORE* Store,
    unsigned char* Buffer,
    size_t BufferSize,
    int* NeedsManufacture)
{
    unsigned char header[SOFTTPM_STATE_HEADER_SIZE];
    unsigned char* fileData = NULL;
    DWORD read = 0;
    HANDLE file;
    LARGE_INTEGER size;
    int status;
    unsigned short flags;
    unsigned int storedSize;

    *NeedsManufacture = 0;

    file = CreateFileW(Store->StatePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            log_info("state: no TPM state file; manufacturing a new TPM");
            memset(Buffer, 0xFF, BufferSize);
            *NeedsManufacture = 1;
            return 0;
        }

        log_error("state: cannot open %ls (error %lu)", Store->StatePath, GetLastError());
        return -1;
    }

    if (!GetFileSizeEx(file, &size) || size.QuadPart != (LONGLONG)(SOFTTPM_STATE_HEADER_SIZE + BufferSize))
    {
        if (!GetFileSizeEx(file, &size))
        {
            log_error("state: cannot determine size of %ls", Store->StatePath);
        }
        else
        {
            log_error(
                "state: %ls has unexpected size %lld (expected %llu); refusing to continue",
                Store->StatePath,
                size.QuadPart,
                (unsigned long long)(SOFTTPM_STATE_HEADER_SIZE + BufferSize));
        }

        CloseHandle(file);
        return -1;
    }

    fileData = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (fileData == NULL)
    {
        CloseHandle(file);
        return -1;
    }

    if (!ReadFile(file, fileData, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart)
    {
        log_error("state: cannot read %ls (error %lu)", Store->StatePath, GetLastError());
        HeapFree(GetProcessHeap(), 0, fileData);
        CloseHandle(file);
        return -1;
    }

    CloseHandle(file);

    memcpy(header, fileData, SOFTTPM_STATE_HEADER_SIZE);

    if (memcmp(header, SOFTTPM_STATE_MAGIC, SOFTTPM_STATE_MAGIC_SIZE) != 0
        || ReadLe16(header + 8) != SOFTTPM_STATE_VERSION)
    {
        log_error("state: %ls is not a valid SoftTPM state file", Store->StatePath);
        HeapFree(GetProcessHeap(), 0, fileData);
        return -1;
    }

    flags = ReadLe16(header + 10);
    storedSize = ReadLe32(header + 12);
    Store->Sequence = ReadLe32(header + 16);

    if (storedSize != BufferSize)
    {
        log_error(
            "state: %ls stores a %u byte image but this build expects %llu bytes",
            Store->StatePath,
            storedSize,
            (unsigned long long)BufferSize);
        HeapFree(GetProcessHeap(), 0, fileData);
        return -1;
    }

    if (!Store->KeyLoaded)
    {
        log_error("state: no state key loaded");
        HeapFree(GetProcessHeap(), 0, fileData);
        return -1;
    }

    status = softtpm_aes_gcm_decrypt(
        Store->Key,
        header + 20,
        header,
        SOFTTPM_STATE_AAD_SIZE,
        fileData + SOFTTPM_STATE_HEADER_SIZE,
        BufferSize,
        header + 32,
        Buffer);

    SecureZeroMemory(fileData, (SIZE_T)size.QuadPart);
    HeapFree(GetProcessHeap(), 0, fileData);

    if (status != 0)
    {
        log_error(
            "state: cannot authenticate %ls; the state was modified or the key does not match",
            Store->StatePath);
        return -1;
    }

    *NeedsManufacture = (flags & SOFTTPM_STATE_FLAG_MANUFACTURED) == 0;
    return 0;
}

int state_store_save_nv(
    SOFTTPM_STATE_STORE* Store,
    const unsigned char* Buffer,
    size_t BufferSize,
    int Manufactured)
{
    unsigned char* fileData;
    size_t fileSize = SOFTTPM_STATE_HEADER_SIZE + BufferSize;
    int status;

    if (!Store->KeyLoaded)
    {
        log_error("state: no state key loaded");
        return -1;
    }

    fileData = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, fileSize);
    if (fileData == NULL)
    {
        return -1;
    }

    memcpy(fileData, SOFTTPM_STATE_MAGIC, SOFTTPM_STATE_MAGIC_SIZE);
    WriteLe16(fileData + 8, SOFTTPM_STATE_VERSION);
    WriteLe16(
        fileData + 10,
        Manufactured ? SOFTTPM_STATE_FLAG_MANUFACTURED : 0);
    WriteLe32(fileData + 12, (unsigned int)BufferSize);
    WriteLe32(fileData + 16, ++Store->Sequence);

    if (softtpm_random(fileData + 20, SOFTTPM_STATE_NONCE_SIZE) != 0)
    {
        HeapFree(GetProcessHeap(), 0, fileData);
        return -1;
    }

    status = softtpm_aes_gcm_encrypt(
        Store->Key,
        fileData + 20,
        fileData,
        SOFTTPM_STATE_AAD_SIZE,
        Buffer,
        BufferSize,
        fileData + SOFTTPM_STATE_HEADER_SIZE,
        fileData + 32);

    if (status == 0)
    {
        status = WriteFileAtomic(Store->StatePath, fileData, fileSize);
    }

    SecureZeroMemory(fileData, fileSize);
    HeapFree(GetProcessHeap(), 0, fileData);

    return status;
}

int state_store_reset(SOFTTPM_STATE_STORE* Store)
{
    DeleteFileW(Store->StatePath);
    DeleteFileW(Store->KeyPath);
    SecureZeroMemory(Store->Key, sizeof(Store->Key));
    Store->KeyLoaded = 0;
    Store->Sequence = 0;
    return 0;
}
