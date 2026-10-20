#ifndef SOFTTPM_ENGINE_H
#define SOFTTPM_ENGINE_H

#include <stdint.h>
#include <stddef.h>

int engine_start(const wchar_t* StateDirectory);
void engine_stop(void);
int engine_execute(
    const unsigned char* Command,
    uint32_t CommandSize,
    unsigned char* Response,
    uint32_t* ResponseSize);

int engine_execute_locality(
    const unsigned char* Command,
    uint32_t CommandSize,
    uint32_t Locality,
    unsigned char* Response,
    uint32_t* ResponseSize);

#endif
