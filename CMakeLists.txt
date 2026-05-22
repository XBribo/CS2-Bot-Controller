cmake_minimum_required(VERSION 3.20)
project(BotWeaponLock CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BotWeaponLock requires x64 build (use -A x64).")
endif()

set(HL2SDK $ENV{HL2SDKCS2})
set(MMS    $ENV{MMSOURCE_DEV})
set(PROTO_DIR $ENV{CSGO_PROTO})
foreach(v HL2SDK MMS PROTO_DIR)
    if(NOT ${v} OR NOT EXISTS "${${v}}")
        message(FATAL_ERROR "${v} env var not set or path missing: '${${v}}'")
    endif()
endforeach()

# eiface.h includes network_connection.pb.h; we only need the generated
# header to exist (no symbols used directly).
if(DEFINED ENV{PROTOC} AND EXISTS "$ENV{PROTOC}")
    set(PROTOC_EXE "$ENV{PROTOC}")
else()
    find_program(PROTOC_EXE protoc REQUIRED)
endif()
execute_process(COMMAND "${PROTOC_EXE}" --version OUTPUT_VARIABLE PROTOC_VER OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT PROTOC_VER MATCHES "libprotoc 3\\.21\\.")
    message(FATAL_ERROR "protoc must be 3.21.x (got '${PROTOC_VER}')")
endif()
message(STATUS "protoc: ${PROTOC_EXE} (${PROTOC_VER})")

set(PROTO_FILES network_connection.proto)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(PROTO_SRCS "")
set(PROTO_INPUTS "")
foreach(p ${PROTO_FILES})
    string(REGEX REPLACE "\\.proto$" ".pb.cc" cc ${p})
    string(REGEX REPLACE "\\.proto$" ".pb.h"  hh ${p})
    list(APPEND PROTO_SRCS   "${GENERATED_DIR}/${cc}" "${GENERATED_DIR}/${hh}")
    list(APPEND PROTO_INPUTS "${PROTO_DIR}/${p}")
endforeach()
add_custom_command(
    OUTPUT ${PROTO_SRCS}
    COMMAND "${PROTOC_EXE}" --cpp_out=${GENERATED_DIR} --proto_path=${PROTO_DIR} ${PROTO_FILES}
    DEPENDS ${PROTO_INPUTS}
    WORKING_DIRECTORY "${PROTO_DIR}"
    COMMENT "protoc ${PROTO_FILES}"
    VERBATIM
)

include(FetchContent)
if(DEFINED ENV{MINHOOK_SOURCE} AND EXISTS "$ENV{MINHOOK_SOURCE}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_MINHOOK "$ENV{MINHOOK_SOURCE}" CACHE PATH "")
endif()
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook)

set(SDK_SOURCES
    ${HL2SDK}/public/tier0/memoverride.cpp
    ${HL2SDK}/tier1/generichash.cpp
    ${HL2SDK}/tier1/keyvalues3.cpp
    ${HL2SDK}/tier1/convar.cpp
    ${MMS}/core/sourcehook/sourcehook.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookidman.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookmaninfo.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cvfnptr.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cproto.cpp
)

add_library(BotWeaponLock SHARED
    src/plugin.cpp
    src/hooks.cpp
    src/sig_scan.cpp
    src/lock_state.cpp
    src/ccsbot_slot.cpp
    src/dispatch.cpp
    src/commands.cpp
    src/exports.cpp
    ${PROTO_SRCS}
    ${SDK_SOURCES}
)

target_include_directories(BotWeaponLock PRIVATE
    src
    ${GENERATED_DIR}
    ${HL2SDK}/thirdparty/protobuf-3.21.8/src
    ${MMS}/core ${MMS}/core/sourcehook ${MMS}/core/sourcemm
    ${HL2SDK}/public
    ${HL2SDK}/public/engine
    ${HL2SDK}/public/game/server
    ${HL2SDK}/public/game/shared
    ${HL2SDK}/public/mathlib
    ${HL2SDK}/public/tier0
    ${HL2SDK}/public/tier1
    ${HL2SDK}/public/entity2
    ${HL2SDK}/game/shared
    ${HL2SDK}/common
)

set(HL2SDK_LIBDIR ${HL2SDK}/lib/public/win64)
target_link_libraries(BotWeaponLock PRIVATE
    minhook psapi
    ${HL2SDK_LIBDIR}/tier0.lib
    ${HL2SDK_LIBDIR}/tier1.lib
    ${HL2SDK_LIBDIR}/mathlib.lib
    ${HL2SDK_LIBDIR}/interfaces.lib
    ${HL2SDK_LIBDIR}/2015/libprotobuf.lib
)
target_link_options(BotWeaponLock PRIVATE /NODEFAULTLIB:LIBCMTD)

target_compile_definitions(BotWeaponLock PRIVATE
    META_IS_SOURCE2 _CRT_SECURE_NO_WARNINGS
    WIN32 _WIN32 _WINDOWS
    COMPILER_MSVC COMPILER_MSVC64
    PLATFORM_64BITS X64BITS
    NOMINMAX _ALLOW_KEYWORD_MACROS
    GOOGLE_PROTOBUF_NO_RTTI
)

set_target_properties(BotWeaponLock PROPERTIES OUTPUT_NAME "BotWeaponLock" PREFIX "" SUFFIX ".dll")

if(MSVC)
    target_compile_options(BotWeaponLock PRIVATE
        /W3 /permissive- /wd4819 /wd4267 /wd4244 /wd4146 /wd4996 /wd4661)
endif()
