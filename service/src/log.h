#ifndef SOFTTPM_LOG_H
#define SOFTTPM_LOG_H

typedef enum _SOFTTPM_LOG_LEVEL
{
    SOFTTPM_LOG_DEBUG = 0,
    SOFTTPM_LOG_INFO = 1,
    SOFTTPM_LOG_WARN = 2,
    SOFTTPM_LOG_ERROR = 3,
    SOFTTPM_LOG_NONE = 4
} SOFTTPM_LOG_LEVEL;

void log_init(const wchar_t* FilePath, SOFTTPM_LOG_LEVEL Level, int AlsoConsole);
void log_close(void);
void log_set_level(SOFTTPM_LOG_LEVEL Level);

void log_debug(const char* Format, ...);
void log_info(const char* Format, ...);
void log_warn(const char* Format, ...);
void log_error(const char* Format, ...);

#endif
