#ifndef SOFTTPM_STATE_STORE_H
#define SOFTTPM_STATE_STORE_H

#include <stddef.h>
#include "state_crypto.h"

#define SOFTTPM_STATE_MAX_PATH 512

typedef struct _SOFTTPM_STATE_STORE
{
    wchar_t Directory[SOFTTPM_STATE_MAX_PATH];
    wchar_t KeyPath[SOFTTPM_STATE_MAX_PATH];
    wchar_t StatePath[SOFTTPM_STATE_MAX_PATH];
    unsigned char Key[SOFTTPM_STATE_KEY_SIZE];
    unsigned int Sequence;
    int KeyLoaded;
} SOFTTPM_STATE_STORE;

int state_store_open(SOFTTPM_STATE_STORE* Store, const wchar_t* Directory);

int state_store_load_nv(
    SOFTTPM_STATE_STORE* Store,
    unsigned char* Buffer,
    size_t BufferSize,
    int* NeedsManufacture);

int state_store_save_nv(
    SOFTTPM_STATE_STORE* Store,
    const unsigned char* Buffer,
    size_t BufferSize,
    int Manufactured);

int state_store_reset(SOFTTPM_STATE_STORE* Store);

const wchar_t* state_store_directory(const SOFTTPM_STATE_STORE* Store);

#endif
