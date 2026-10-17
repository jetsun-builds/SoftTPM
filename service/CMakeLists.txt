cmake_minimum_required(VERSION 3.16.3)

project(SoftTpmService C)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release"
    CACHE STRING "Configuration types" FORCE)

include(GNUInstallDirs)

if (NOT DEFINED SoftTpm_TpmPrefix)
    message(FATAL_ERROR "SoftTpm_TpmPrefix must point at the installed ms-tpm-20-ref prefix")
endif()
if (NOT DEFINED SoftTpm_OpenSslRoot)
    message(FATAL_ERROR "SoftTpm_OpenSslRoot must point at the OpenSSL installation")
endif()

find_package(Tpm_CompilerOptions PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(TpmConfiguration CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_Public_Headers CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_Platform_Interface CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_CryptoLib_Common PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_CryptoLib_TpmBigNum_Headers PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_CryptoLib_Math_Ossl CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_CryptoLib_TpmBigNum CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)
find_package(Tpm_CoreLib CONFIG REQUIRED PATHS ${SoftTpm_TpmPrefix} NO_DEFAULT_PATH)

add_executable(SoftTpmService WIN32
    src/main.c
    src/options.c
    src/service_control.c
    src/engine.c
    src/driver_client.c
    src/log.c
    src/storage/state_crypto.c
    src/storage/state_store.c
    src/platform/platform_data.c
    src/platform/platform_nv.c
    src/platform/platform_time.c
    src/platform/platform_run.c
    src/platform/platform_misc.c
    src/platform/platform_pcr.c
    src/platform/platform_act.c
    src/provision/tpm_commands.c
    src/provision/ek_certificate.c
    src/provision/provision.c
)

target_include_directories(SoftTpmService PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
    ${SoftTpm_TpmPrefix}/include
    ${SoftTpm_OpenSslRoot}/include
)

target_compile_definitions(SoftTpmService PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    _WINDOWS
    UNICODE
    _UNICODE
)

target_link_libraries(SoftTpmService PRIVATE
    TpmConfiguration::TpmConfiguration
    Tpm_Public_Headers::Tpm_Public_Headers
    Tpm_Platform_Interface::Tpm_Platform_Interface
    Tpm_CoreLib::Tpm_CoreLib
    Tpm_CryptoLib_TpmBigNum::Tpm_CryptoLib_TpmBigNum
    crypt32
    bcrypt
    advapi32
    shell32
)

if (WIN32)
    if (NOT DEFINED SoftTpm_OpenSslLibDir)
        if (CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(SoftTpm_OpenSslLibDir "${SoftTpm_OpenSslRoot}/lib/x64")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(SoftTpm_OpenSslLibDir "${SoftTpm_OpenSslRoot}/lib")
        else ()
            message(FATAL_ERROR "CMAKE_SIZEOF_VOID_P: ${CMAKE_SIZEOF_VOID_P}")
        endif ()
    endif ()

    if (NOT DEFINED SoftTpm_OpenSslDllDir)
        set(SoftTpm_OpenSslDllDir "${SoftTpm_OpenSslLibDir}")
    endif ()

    target_link_directories(SoftTpmService PRIVATE ${SoftTpm_OpenSslLibDir})
    target_link_libraries(SoftTpmService PRIVATE libcrypto)

    file(GLOB SoftTpm_LibCryptoDlls
        "${SoftTpm_OpenSslDllDir}/*crypto*.dll"
        "${SoftTpm_OpenSslLibDir}/*crypto*.dll")
    if (SoftTpm_LibCryptoDlls)
        add_custom_command(TARGET SoftTpmService POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SoftTpm_LibCryptoDlls} "$<TARGET_FILE_DIR:SoftTpmService>"
            COMMENT "Copying libcrypto runtime DLLs next to SoftTpmService")
    endif ()
else ()
    target_link_libraries(SoftTpmService PRIVATE crypto)
endif ()

install(TARGETS SoftTpmService
        RUNTIME DESTINATION bin)
