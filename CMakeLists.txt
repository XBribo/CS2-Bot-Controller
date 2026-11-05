cmake_minimum_required(VERSION 3.20)
project(BotController VERSION 0.9.0 LANGUAGES CXX)

# Override the base version with -DPLUGIN_VERSION.
if(NOT DEFINED PLUGIN_VERSION)
    set(PLUGIN_VERSION "${PROJECT_VERSION}")
endif()
find_package(Git QUIET)
set(PLUGIN_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/plugin_build_info.h")
add_custom_target(BotController_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_FILE=${PLUGIN_BUILD_INFO}"
        -P "${CMAKE_SOURCE_DIR}/cmake/UpdateBuildInfo.cmake"
    BYPRODUCTS "${PLUGIN_BUILD_INFO}"
    VERBATIM
)
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format.exe)
set(BOT_CONTROLLER_FORMAT_FILES "")
# Format plugin sources only.
foreach(extension c cc cpp cxx h hh hpp hxx inl)
    file(GLOB plugin_format_top CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/*.${extension}")
    file(GLOB_RECURSE plugin_format_nested CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/src/features/*.${extension}" "${CMAKE_SOURCE_DIR}/src/bridge/*.${extension}"
        "${CMAKE_SOURCE_DIR}/src/utils/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/entity/*.${extension}")
    list(APPEND BOT_CONTROLLER_FORMAT_FILES ${plugin_format_top} ${plugin_format_nested})
endforeach()

option(ENABLE_FORMAT_CHECK "Run clang-format check before building BotController" OFF)

if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${BOT_CONTROLLER_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting BotController sources"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file ${BOT_CONTROLLER_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking BotController source formatting"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

add_subdirectory(src/core/cs2_sdk)

add_library(
    BotController SHARED
    src/plugin.cpp
    src/core/interfaces.cpp
    src/core/gamedata.cpp
    src/features/weapons/WeaponLocker.cpp
    src/features/weapons/WeaponLockerState.cpp
    src/features/controller/BotController.cpp
    src/features/controller/BotControllerState.cpp
    src/features/buy/BuyController.cpp
    src/features/buy/BuyControllerState.cpp
    src/features/recorder/InputInjector.cpp
    src/features/recorder/CommandHistory.cpp
    src/features/recorder/PawnBinding.cpp
    src/features/recorder/MotionRecorder.cpp
    src/features/voice/VoiceSender.cpp
    src/features/profile/BotProfile.cpp
    src/core/offsets.cpp
    src/core/entity/ccsbot_slot.cpp
    src/bridge/dispatch.cpp
    src/core/commands.cpp
    src/bridge/exports.cpp
)

target_link_libraries(BotController PRIVATE cs2_sdk::sdk)
cs2_sdk_configure_plugin(BotController)
add_dependencies(BotController BotController_build_info)

if(ENABLE_FORMAT_CHECK)
    add_dependencies(BotController format-check)
endif()

target_include_directories(BotController PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    src
    src/core
    src/core/entity
    src/bridge
    src/utils
    src/features/weapons
    src/features/controller
    src/features/buy
    src/features/recorder
    src/features/voice
    src/features/profile
)

target_compile_definitions(
    BotController
    PRIVATE
    SEMVER="${PLUGIN_VERSION}"
)

if(WIN32)
    set(BC_PLATFORM_DIR win64)
    set(BC_SUFFIX ".dll")
else()
    set(BC_PLATFORM_DIR linuxsteamrt64)
    set(BC_SUFFIX ".so")
endif()

set(BC_PACKAGE_BIN_DIR "${CMAKE_BINARY_DIR}/package/addons/BotController/bin/${BC_PLATFORM_DIR}")
set(BC_VDF_FILE "addons/BotController/bin/${BC_PLATFORM_DIR}/BotController")

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/BotController.vdf.in"
    "${CMAKE_BINARY_DIR}/BotController.vdf"
    @ONLY
)

set_target_properties(
    BotController
    PROPERTIES
    OUTPUT_NAME "BotController"
    PREFIX ""
    SUFFIX "${BC_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY         "${BC_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BC_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${BC_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY         "${BC_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${BC_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${BC_PACKAGE_BIN_DIR}"
)

# Stage the Metamod entry, gamedata, and command permissions.
add_custom_target(BotController_resources
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/metamod"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/BotController"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/BotController.vdf"
        "${CMAKE_BINARY_DIR}/package/addons/metamod/BotController.vdf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotController/gamedata.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotController/gamedata.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotController/config.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotController/config.json"
    VERBATIM
)
add_dependencies(BotController BotController_resources)
