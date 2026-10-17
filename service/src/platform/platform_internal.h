#ifndef SOFTTPM_PLATFORM_INTERNAL_H
#define SOFTTPM_PLATFORM_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <TpmConfiguration/TpmBuildSwitches.h>
#include <TpmConfiguration/TpmProfile.h>
#include <public/BaseTypes.h>
#include <public/TpmTypes.h>
#include <public/TPMB.h>
#include <public/MinMax.h>
#include <public/ACT.h>

#include <platform_interface/tpm_to_platform_interface.h>
#include <platform_interface/platform_to_tpm_interface.h>
#include <platform_interface/prototypes/platform_pcr_fp.h>
#include <platform_interface/prototypes/ExecCommand_fp.h>
#include <platform_interface/prototypes/Manufacture_fp.h>
#include <platform_interface/prototypes/_TPM_Init_fp.h>

#define GLOBAL_C
#define NV_C
#include <platform_interface/pcrstruct.h>

#include "log.h"
#include "storage/state_store.h"

#ifdef _PLATFORM_DATA_C_
#  define PLATFORM_EXTERN
#else
#  define PLATFORM_EXTERN extern
#endif

typedef struct
{
    uint32_t remaining;
    uint32_t newValue;
    uint8_t signaled;
    uint8_t pending;
    uint8_t number;
} ACT_DATA, *P_ACT_DATA;

PLATFORM_EXTERN int s_isCanceled;

PLATFORM_EXTERN uint64_t s_realTimePrevious;
PLATFORM_EXTERN uint64_t s_lastSystemTime;
PLATFORM_EXTERN uint64_t s_lastReportedTime;
PLATFORM_EXTERN uint64_t s_tpmTime;

PLATFORM_EXTERN int s_timerReset;
PLATFORM_EXTERN int s_timerStopped;
PLATFORM_EXTERN uint64_t s_initClock;
PLATFORM_EXTERN unsigned int s_adjustRate;

PLATFORM_EXTERN unsigned char s_locality;

PLATFORM_EXTERN unsigned char s_NV[NV_MEMORY_SIZE];
PLATFORM_EXTERN int s_NvIsAvailable;
PLATFORM_EXTERN int s_NV_unrecoverable;
PLATFORM_EXTERN int s_NV_recoverable;

PLATFORM_EXTERN int s_physicalPresence;

PLATFORM_EXTERN int s_powerLost;

PLATFORM_EXTERN uint32_t lastEntropy;

#define PLATFORM_DECLARE_ACT(N) PLATFORM_EXTERN ACT_DATA ACT_##N;
FOR_EACH_ACT(PLATFORM_DECLARE_ACT)
#undef PLATFORM_DECLARE_ACT

PLATFORM_EXTERN int actTicksAllowed;

LIB_EXPORT void _plat__SetCancel(void);
LIB_EXPORT void _plat__ClearCancel(void);
LIB_EXPORT void _plat__TimerReset(void);
LIB_EXPORT void _plat__TimerRestart(void);
LIB_EXPORT void _plat__LocalitySet(unsigned char locality);
LIB_EXPORT void _plat__NVDisable(void* platParameter, size_t paramSize);
LIB_EXPORT void _plat__SetNvAvail(void);
LIB_EXPORT void _plat__ClearNvAvail(void);
LIB_EXPORT void _plat__NvErrors(int recoverable, int unrecoverable);
LIB_EXPORT int _plat__NVNeedsManufacture(void);
LIB_EXPORT int _plat__Signal_PowerOn(void);
LIB_EXPORT int _plat__Signal_Reset(void);
LIB_EXPORT void _plat__Signal_PowerOff(void);
LIB_EXPORT void _plat__Signal_PhysicalPresenceOn(void);
LIB_EXPORT void _plat__Signal_PhysicalPresenceOff(void);

LIB_EXPORT void _plat__RunCommand(
    uint32_t requestSize,
    unsigned char* request,
    uint32_t* responseSize,
    unsigned char** response);

LIB_EXPORT int _plat__ACT_GetPending(uint32_t act);
LIB_EXPORT void _plat__ACT_Tick(void);

int platform_nv_configure(SOFTTPM_STATE_STORE* Store);
void platform_nv_mark_manufactured(void);

#endif
