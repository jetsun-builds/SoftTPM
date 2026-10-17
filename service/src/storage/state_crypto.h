#ifndef SOFTTPM_STATE_CRYPTO_H
#define SOFTTPM_STATE_CRYPTO_H

#include <stddef.h>

#define SOFTTPM_STATE_KEY_SIZE 32
#define SOFTTPM_STATE_NONCE_SIZE 12
#define SOFTTPM_STATE_TAG_SIZE 16

int softtpm_random(unsigned char* Buffer, size_t Size);

int softtpm_sha256(
    const unsigned char* Data,
    size_t DataSize,
    unsigned char Digest[SOFTTPM_STATE_KEY_SIZE]);

int softtpm_machine_entropy(unsigned char Digest[SOFTTPM_STATE_KEY_SIZE]);

int softtpm_protect_key(
    const unsigned char* Key,
    size_t KeySize,
    unsigned char** Blob,
    size_t* BlobSize);

int softtpm_unprotect_key(
    const unsigned char* Blob,
    size_t BlobSize,
    unsigned char* Key,
    size_t KeySize);

int softtpm_aes_gcm_encrypt(
    const unsigned char Key[SOFTTPM_STATE_KEY_SIZE],
    const unsigned char Nonce[SOFTTPM_STATE_NONCE_SIZE],
    const unsigned char* Aad,
    size_t AadSize,
    const unsigned char* Plain,
    size_t PlainSize,
    unsigned char* Cipher,
    unsigned char Tag[SOFTTPM_STATE_TAG_SIZE]);

int softtpm_aes_gcm_decrypt(
    const unsigned char Key[SOFTTPM_STATE_KEY_SIZE],
    const unsigned char Nonce[SOFTTPM_STATE_NONCE_SIZE],
    const unsigned char* Aad,
    size_t AadSize,
    const unsigned char* Cipher,
    size_t CipherSize,
    const unsigned char Tag[SOFTTPM_STATE_TAG_SIZE],
    unsigned char* Plain);

#endif
