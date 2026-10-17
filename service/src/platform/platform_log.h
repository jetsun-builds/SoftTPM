#ifndef SOFTTPM_PLATFORM_LOG_H
#define SOFTTPM_PLATFORM_LOG_H

#include <stdint.h>

int platform_log_load_and_replay(const wchar_t* Path);
const unsigned char* platform_log_data(uint32_t* Size);

#endif
