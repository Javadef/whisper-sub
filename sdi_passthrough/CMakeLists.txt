cmake_minimum_required(VERSION 3.16)
project(sdi_passthrough CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- AJA NTV2 SDK location -----------------------------------------------
# If you built libajantv2 from source (https://github.com/aja-video/libajantv2)
# point AJA_SDK_ROOT at the repo root.
# Example: cmake -DAJA_SDK_ROOT=C:/aja/libajantv2 ..
set(AJA_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libajantv2" CACHE PATH "Root of AJA NTV2 SDK / libajantv2 repo")

find_path(NTV2_INCLUDE_DIR
    NAMES ntv2card.h
    HINTS
        "${AJA_SDK_ROOT}/ajantv2/includes"
        "${AJA_SDK_ROOT}/includes"
        "C:/Program Files/AJA/SDK/includes"
)

find_library(NTV2_LIBRARY
    NAMES ajantv2 libajantv2
    HINTS
        "${AJA_SDK_ROOT}/build/ajantv2/Release"
        "${AJA_SDK_ROOT}/build/ajantv2/Debug"
        "${AJA_SDK_ROOT}/build/ajantv2"
        "${AJA_SDK_ROOT}/build/Release"
        "${AJA_SDK_ROOT}/lib"
        "C:/Program Files/AJA/SDK/lib"
)

find_path(AJA_BASE_INCLUDE_DIR
    NAMES ajabase/system/systemtime.h
    HINTS
        "${AJA_SDK_ROOT}"
        "${AJA_SDK_ROOT}/ajabase"
        "C:/Program Files/AJA/SDK"
)

if(NOT NTV2_INCLUDE_DIR)
    message(FATAL_ERROR
        "AJA NTV2 headers not found.\n"
        "Clone https://github.com/aja-video/libajantv2 and build it, "
        "then set -DAJA_SDK_ROOT=<path>.")
endif()

if(NOT NTV2_LIBRARY)
    message(FATAL_ERROR
        "AJA NTV2 library not found. Build libajantv2 first, "
        "then set -DAJA_SDK_ROOT=<path>.")
endif()

add_executable(sdi_passthrough main.cpp)

target_include_directories(sdi_passthrough PRIVATE
    ${NTV2_INCLUDE_DIR}
    ${AJA_BASE_INCLUDE_DIR}
    "${AJA_SDK_ROOT}/ajantv2/src/win"
    "${AJA_SDK_ROOT}/ajantv2/src"
)

target_link_libraries(sdi_passthrough PRIVATE ${NTV2_LIBRARY})

# mbedtls (built alongside ajantv2)
set(MBEDTLS_LIB_DIR "${AJA_SDK_ROOT}/build/ajantv2/mbedtls-install/lib")
target_link_libraries(sdi_passthrough PRIVATE
    "${MBEDTLS_LIB_DIR}/mbedtls.lib"
    "${MBEDTLS_LIB_DIR}/mbedcrypto.lib"
    "${MBEDTLS_LIB_DIR}/mbedx509.lib"
    "${MBEDTLS_LIB_DIR}/everest.lib"
    "${MBEDTLS_LIB_DIR}/p256m.lib"
)

if(WIN32)
    target_link_libraries(sdi_passthrough PRIVATE
        winmm setupapi Ws2_32 Netapi32 Shlwapi wbemuuid bcrypt
    )
    target_compile_definitions(sdi_passthrough PRIVATE AJA_WINDOWS MSWindows)
endif()
