# SoftTPM: prepares the vendored ms-tpm-20-ref tree for the build: produces a
# customized TpmConfiguration directory and relaxes the OpenSSL version gate
# checked by the Ossl cryptolib. Invoked via "cmake -P" from scripts/build.ps1.
# Runs are idempotent: unchanged outputs are not rewritten, so incremental
# rebuilds stay incremental.

if (NOT DEFINED SoftTpm_TpmSourceDir)
    message(FATAL_ERROR "SoftTpm_TpmSourceDir is required")
endif()
if (NOT DEFINED SoftTpm_ConfigOutDir)
    message(FATAL_ERROR "SoftTpm_ConfigOutDir is required")
endif()

set(SoftTpm_TpmSourceDir "${SoftTpm_TpmSourceDir}")
set(SoftTpm_ConfigOutDir "${SoftTpm_ConfigOutDir}")

if (NOT EXISTS "${SoftTpm_TpmSourceDir}/TpmConfiguration/CMakeLists.txt")
    message(FATAL_ERROR "TpmConfiguration not found under ${SoftTpm_TpmSourceDir}")
endif()

set(OsslMathHeader "${SoftTpm_TpmSourceDir}/tpm/cryptolibs/Ossl/include/Ossl/BnToOsslMath.h")
if (NOT EXISTS "${OsslMathHeader}")
    message(FATAL_ERROR "BnToOsslMath.h not found under ${SoftTpm_TpmSourceDir}")
endif()

file(READ "${OsslMathHeader}" OsslMath)
string(REPLACE
    "#if OPENSSL_VERSION_NUMBER >= 0x30100000L\n// Check the bignum_st definition against the one below and either update the\n// version check or provide the new definition for this version.\n#  error Untested OpenSSL version\n#elif OPENSSL_VERSION_NUMBER >= 0x10100000L"
    "#if OPENSSL_VERSION_NUMBER >= 0x10100000L"
    OsslMathPatched "${OsslMath}")
if (NOT OsslMathPatched STREQUAL "${OsslMath}")
    file(WRITE "${OsslMathHeader}" "${OsslMathPatched}")
    set(OsslMath "${OsslMathPatched}")
endif()

string(FIND "${OsslMath}" "Untested OpenSSL version" OsslGate)
if (NOT OsslGate EQUAL -1)
    message(FATAL_ERROR "Failed to relax the OpenSSL version gate in BnToOsslMath.h")
endif()
string(FIND "${OsslMath}" "#if OPENSSL_VERSION_NUMBER >= 0x10100000L" OsslMerged)
if (OsslMerged EQUAL -1)
    message(FATAL_ERROR "BnToOsslMath.h does not contain the expected OpenSSL version gate")
endif()

set(SwitchesFile "${SoftTpm_ConfigOutDir}/TpmConfiguration/TpmBuildSwitches.h")
set(ProfileFile "${SoftTpm_ConfigOutDir}/TpmConfiguration/TpmProfile_Misc.h")
set(ProfileCommonFile "${SoftTpm_ConfigOutDir}/TpmConfiguration/TpmProfile_Common.h")
set(VersionFile "${SoftTpm_ConfigOutDir}/SoftTpmPrepare.version")

# Bump this whenever the patches below change so that existing build trees are
# refreshed instead of silently keeping the previous configuration.
set(SoftTpmPrepareVersion "3")

set(AlreadyPrepared FALSE)
if (EXISTS "${SwitchesFile}" AND EXISTS "${VersionFile}")
    file(READ "${SwitchesFile}" ExistingSwitches)
    string(FIND "${ExistingSwitches}" "#define SIMULATION                  NO" Found)
    file(READ "${VersionFile}" PreparedVersion)
    string(STRIP "${PreparedVersion}" PreparedVersion)
    if (NOT Found EQUAL -1 AND PreparedVersion STREQUAL SoftTpmPrepareVersion)
        set(AlreadyPrepared TRUE)
    endif()
endif()

if (NOT AlreadyPrepared)
    if (EXISTS "${SoftTpm_ConfigOutDir}")
        file(REMOVE_RECURSE "${SoftTpm_ConfigOutDir}")
    endif()

    # Copy the *contents* of TPMCmd/TpmConfiguration so that the output
    # directory itself contains CMakeLists.txt and the TpmConfiguration header
    # directory. This mirrors the layout the reference build expects for
    # user_TpmConfiguration_Dir (add_subdirectory of that directory).
    file(COPY "${SoftTpm_TpmSourceDir}/TpmConfiguration/" DESTINATION "${SoftTpm_ConfigOutDir}")

    file(READ "${SwitchesFile}" Switches)
    string(REPLACE
        "#define SIMULATION                  YES"
        "#define SIMULATION                  NO"
        Switches "${Switches}")
    string(REPLACE
        "#define CRYPTO_LIB_REPORTING        YES"
        "#define CRYPTO_LIB_REPORTING        NO"
        Switches "${Switches}")
    string(REPLACE
        "#define ALLOW_FORCE_FAILURE_MODE    YES"
        "#define ALLOW_FORCE_FAILURE_MODE    NO"
        Switches "${Switches}")
    file(WRITE "${SwitchesFile}" "${Switches}")

    file(READ "${ProfileFile}" Profile)
    string(REPLACE
        "#define NV_MEMORY_SIZE             16384"
        "#define NV_MEMORY_SIZE             32768"
        Profile "${Profile}")
    # The reference default of three transient objects is too small for the
    # Windows TPM stack: tpmtool's ekhelper keeps SRK, EK and certificate
    # objects loaded at once and fails with TPM_RC_OBJECT_MEMORY (0x902).
    string(REPLACE
        "#define MAX_LOADED_OBJECTS         3"
        "#define MAX_LOADED_OBJECTS         8"
        Profile "${Profile}")
    file(WRITE "${ProfileFile}" "${Profile}")

    file(READ "${ProfileCommonFile}" ProfileCommon)
    string(REGEX REPLACE
        "#define ALG_SHA512([ \t]+)ALG_NO"
        "#define ALG_SHA512\\1ALG_YES"
        ProfileCommon "${ProfileCommon}")
    file(WRITE "${ProfileCommonFile}" "${ProfileCommon}")

    file(READ "${SwitchesFile}" Switches)
    foreach (Expected
        "#define SIMULATION                  NO"
        "#define CRYPTO_LIB_REPORTING        NO"
        "#define ALLOW_FORCE_FAILURE_MODE    NO")
        string(FIND "${Switches}" "${Expected}" Found)
        if (Found EQUAL -1)
            message(FATAL_ERROR "Patch failed, missing: ${Expected}")
        endif()
    endforeach()

    file(READ "${ProfileFile}" Profile)
    string(FIND "${Profile}" "#define NV_MEMORY_SIZE             32768" Found)
    if (Found EQUAL -1)
        message(FATAL_ERROR "Patch failed, missing: NV_MEMORY_SIZE 32768")
    endif()
    string(FIND "${Profile}" "#define MAX_LOADED_OBJECTS         8" Found)
    if (Found EQUAL -1)
        message(FATAL_ERROR "Patch failed, missing: MAX_LOADED_OBJECTS 8")
    endif()

    file(READ "${ProfileCommonFile}" ProfileCommon)
    string(REGEX MATCH "#define ALG_SHA512[ \t]+ALG_YES" Found "${ProfileCommon}")
    if (Found STREQUAL "")
        message(FATAL_ERROR "Patch failed, missing: ALG_SHA512 ALG_YES")
    endif()

    file(WRITE "${VersionFile}" "${SoftTpmPrepareVersion}\n")
endif()

if (AlreadyPrepared)
    message(STATUS "SoftTPM TpmConfiguration already prepared at ${SoftTpm_ConfigOutDir}")
else()
    message(STATUS "SoftTPM TpmConfiguration prepared at ${SoftTpm_ConfigOutDir}")
endif()
