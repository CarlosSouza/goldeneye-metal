
# ge - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ge LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED REXSDK_DIR OR REXSDK_DIR STREQUAL "")
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
        "Path to the GoldenEye Metal SDK source tree" FORCE)
endif()

# Keep the SDK artifacts owned by this app configuration inside its own build
# tree. In particular, release and developer-harness builds must never share a
# librexruntime.dylib output path, because either one may be newer while having
# materially different compiled capabilities.
if(NOT DEFINED REXGLUE_OUTPUT_DIRECTORY)
    set(REXGLUE_OUTPUT_DIRECTORY
        "${CMAKE_BINARY_DIR}/rexglue-output"
        CACHE PATH "Build-tree-local ReXGlue artifacts")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR
        "generated/rexglue.cmake is missing. Put your legally-owned Xbox 360 XEX at "
        "assets/default.xex, then run: rexglue codegen ge_manifest.toml")
endif()
include(generated/rexglue.cmake)

# Sources
set(GE_SOURCES
    src/main.cpp
    src/ge_game_data.cpp
    src/ge_gpu_capture.cpp
    src/ge_host_pause.cpp
    src/ge_hooks.cpp
    src/ge_menu.cpp
    src/ge_postfx.cpp
    src/ge_save_manager.cpp
    src/ge_testing_tools.cpp
    src/ge_username.cpp
    src/ge_ce_patches.cpp
)

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND GE_SOURCES
        src/ge_launch_recovery.cpp
        src/ge_launcher_macos.mm
    )
else()
    # iOS is launcherless: game data arrives via the app's Documents folder,
    # either as a ready-made Game Data tree or as a raw backup package the
    # setup screen imports on-device (see windowed_app_main_ios.mm).
    list(APPEND GE_SOURCES
        src/ge_game_data_import_ios.cpp
        src/ge_launcher_stub.cpp
    )
endif()

if(WIN32)
    add_executable(ge WIN32 ${GE_SOURCES})
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output a GoldenEye.exe (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye")

rexglue_setup_target(ge)
if(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
    # The SDK option is directory-scoped when ReXGlue is included as a
    # subproject, so opt this game target into the matching read-only markers.
    target_compile_definitions(ge PRIVATE
        REXGLUE_ENABLE_INPUT_TEST_HARNESS=1)
endif()
if(APPLE)
    find_library(GOLDENEYE_UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK
        UniformTypeIdentifiers REQUIRED)
    target_link_libraries(ge PRIVATE
        "${GOLDENEYE_UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK}")
endif()
if(NOT MSVC)
    target_compile_options(ge PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Filesystem-only regression for the GoldenEye-specific save backup, restore,
# reset, and quarantine/undo backend. It is never packaged.
add_executable(goldeneye_save_manager_test EXCLUDE_FROM_ALL
    tests/save_manager_test.cpp
    src/ge_save_manager.cpp
)
target_include_directories(goldeneye_save_manager_test PRIVATE src)
target_link_libraries(goldeneye_save_manager_test PRIVATE rex::runtime)
target_compile_definitions(goldeneye_save_manager_test PRIVATE
    GOLDENEYE_SAVE_MANAGER_TESTING=1)
rexglue_apply_target_settings(goldeneye_save_manager_test)
if(NOT MSVC)
    target_compile_options(goldeneye_save_manager_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# State-machine regression for the asynchronous host-menu -> retail pause
# bridge. It is source-only and never packaged.
add_executable(goldeneye_host_pause_test EXCLUDE_FROM_ALL
    tests/host_pause_test.cpp
)
target_include_directories(goldeneye_host_pause_test PRIVATE src)
target_compile_definitions(goldeneye_host_pause_test PRIVATE
    REXGLUE_ENABLE_INPUT_TEST_HARNESS=1
    "GOLDENEYE_GENERATED_DIRECTORY=\"${CMAKE_CURRENT_SOURCE_DIR}/generated\"")
rexglue_apply_target_settings(goldeneye_host_pause_test)
if(NOT MSVC)
    target_compile_options(goldeneye_host_pause_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Deterministic regression for the host testing-tools mapping, eligibility
# gate, and mission/pause-bound request queue. It is source-only and never
# packaged.
add_executable(goldeneye_testing_tools_test EXCLUDE_FROM_ALL
    tests/testing_tools_test.cpp
)
target_include_directories(goldeneye_testing_tools_test PRIVATE src)
target_compile_definitions(goldeneye_testing_tools_test PRIVATE
    "GOLDENEYE_TESTING_TOOLS_SOURCE_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/ge_testing_tools.cpp\""
    "GOLDENEYE_GENERATED_DIRECTORY=\"${CMAKE_CURRENT_SOURCE_DIR}/generated\"")
rexglue_apply_target_settings(goldeneye_testing_tools_test)
if(NOT MSVC)
    target_compile_options(goldeneye_testing_tools_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Deterministic regression for the bounded, observation-only player movement
# telemetry used when rendering continues but a player appears unable to move.
add_executable(goldeneye_player_stuck_telemetry_test EXCLUDE_FROM_ALL
    tests/player_stuck_telemetry_test.cpp
)
target_include_directories(goldeneye_player_stuck_telemetry_test PRIVATE src)
rexglue_apply_target_settings(goldeneye_player_stuck_telemetry_test)
if(NOT MSVC)
    target_compile_options(goldeneye_player_stuck_telemetry_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Deterministic regression for the independent guest-present, command-
# processor swap, and ring-progress classifier used by the freeze watchdog.
add_executable(goldeneye_watchdog_test EXCLUDE_FROM_ALL
    tests/watchdog_test.cpp
)
target_include_directories(goldeneye_watchdog_test PRIVATE src)
rexglue_apply_target_settings(goldeneye_watchdog_test)
if(NOT MSVC)
    target_compile_options(goldeneye_watchdog_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Local-only importer smoke tool. It is excluded from normal builds and never
# packaged; developers may invoke it explicitly with their own package to test
# the same validation/extraction backend used by the launcher.
add_executable(goldeneye_game_data_import_smoke EXCLUDE_FROM_ALL
    tests/game_data_import_smoke.cpp
    src/ge_game_data.cpp
)
target_include_directories(goldeneye_game_data_import_smoke PRIVATE src)
target_link_libraries(goldeneye_game_data_import_smoke PRIVATE rex::runtime)
rexglue_apply_target_settings(goldeneye_game_data_import_smoke)
if(NOT MSVC)
    target_compile_options(goldeneye_game_data_import_smoke PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# Deterministic regression for the exact callback target used by the Dam
# packed-data lifetime guard. It is never packaged, but the macOS release gate
# runs it before accepting an app bundle.
add_executable(goldeneye_crash_guard_test EXCLUDE_FROM_ALL
    tests/crash_guard_test.cpp
)
target_include_directories(goldeneye_crash_guard_test PRIVATE src)
if(NOT MSVC)
    target_compile_options(goldeneye_crash_guard_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

add_custom_target(goldeneye_crash_guard_codegen_verify
    COMMAND "${CMAKE_COMMAND}"
        "-DGENERATED_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/generated"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_crash_guard_codegen.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_crash_guard_codegen.cmake"
    COMMENT "Verifying generated Dam purecall guards"
    VERBATIM
)

# Filesystem-only regression for the durable run marker and one-launch Safe
# Mode config snapshot. It is source-only and never packaged.
if(APPLE)
    add_executable(goldeneye_gpu_capture_test EXCLUDE_FROM_ALL
        tests/gpu_capture_test.cpp
        src/ge_gpu_capture.cpp
    )
    target_include_directories(goldeneye_gpu_capture_test PRIVATE src)
    target_link_libraries(goldeneye_gpu_capture_test PRIVATE rex::runtime)
    rexglue_apply_target_settings(goldeneye_gpu_capture_test)
    target_compile_options(goldeneye_gpu_capture_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )

    add_executable(goldeneye_launch_recovery_test EXCLUDE_FROM_ALL
        tests/launch_recovery_test.cpp
        src/ge_launch_recovery.cpp
    )
    target_include_directories(goldeneye_launch_recovery_test PRIVATE src)
    target_compile_options(goldeneye_launch_recovery_test PRIVATE
        "-ffile-prefix-map=${REXSDK_DIR}=."
        "-fmacro-prefix-map=${REXSDK_DIR}=."
    )
endif()

# ---------------------------------------------------------------------------
# Unsigned macOS application bundle.
#
# Keep the developer executable as a normal target (the repository's legacy
# Finder launcher and command-line diagnostics still use it), then stage a
# release-ready .app explicitly with `--target goldeneye_macos_app`.  The
# staged product contains only the host executable, its native runtime dylib,
# metadata, and the source notices.  Game data is deliberately never copied.
# ---------------------------------------------------------------------------
# The staged .app, its verification suite and the launcher are macOS-only;
# iOS packaging is a separate flow (docs/ipad/PLAN.md).
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(GOLDENEYE_RELEASE_MANIFEST
        "${REXSDK_DIR}/config/goldeneye-release.json")
    if(NOT EXISTS "${GOLDENEYE_RELEASE_MANIFEST}")
        message(FATAL_ERROR
            "GoldenEye release manifest is missing: ${GOLDENEYE_RELEASE_MANIFEST}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${GOLDENEYE_RELEASE_MANIFEST}")
    file(READ "${GOLDENEYE_RELEASE_MANIFEST}" _goldeneye_release_json)
    string(JSON GOLDENEYE_RELEASE_VERSION GET
        "${_goldeneye_release_json}" version)
    string(JSON GOLDENEYE_RELEASE_BUILD_NUMBER GET
        "${_goldeneye_release_json}" build)

    # GOLDENEYE_VERSION and GOLDENEYE_BUILD_NUMBER used to be cache entries.
    # Remove those legacy entries on every configure so a developer build can
    # never retain metadata from an older release. Intentional one-run build
    # overrides are environment variables and therefore cannot become sticky.
    unset(GOLDENEYE_VERSION CACHE)
    unset(GOLDENEYE_BUILD_NUMBER CACHE)
    set(GOLDENEYE_VERSION "${GOLDENEYE_RELEASE_VERSION}")
    set(GOLDENEYE_BUILD_NUMBER "${GOLDENEYE_RELEASE_BUILD_NUMBER}")
    if(DEFINED ENV{GOLDENEYE_VERSION_OVERRIDE} AND
       NOT "$ENV{GOLDENEYE_VERSION_OVERRIDE}" STREQUAL "")
        set(GOLDENEYE_VERSION "$ENV{GOLDENEYE_VERSION_OVERRIDE}")
    endif()
    if(DEFINED ENV{GOLDENEYE_BUILD_NUMBER_OVERRIDE} AND
       NOT "$ENV{GOLDENEYE_BUILD_NUMBER_OVERRIDE}" STREQUAL "")
        set(GOLDENEYE_BUILD_NUMBER "$ENV{GOLDENEYE_BUILD_NUMBER_OVERRIDE}")
    endif()
    set(GOLDENEYE_BUNDLE_IDENTIFIER "io.github.ysrdevs.goldeneye-metal" CACHE STRING
        "GoldenEye Metal reverse-DNS bundle identifier")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "The macOS app requires CMAKE_OSX_DEPLOYMENT_TARGET to be set. "
            "Use the macos-arm64-release preset (currently macOS 14.0).")
    endif()
    set(GOLDENEYE_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")

    if(NOT GOLDENEYE_VERSION MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR
            "GOLDENEYE_VERSION must contain two or three numeric components "
            "(for example, 1.2.3)")
    endif()
    if(NOT GOLDENEYE_BUILD_NUMBER MATCHES "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
        message(FATAL_ERROR
            "GOLDENEYE_BUILD_NUMBER must contain one to three numeric components")
    endif()
    if(NOT GOLDENEYE_BUNDLE_IDENTIFIER MATCHES "^[A-Za-z0-9][A-Za-z0-9.-]+$")
        message(FATAL_ERROR "GOLDENEYE_BUNDLE_IDENTIFIER is not a valid bundle identifier")
    endif()

    # A copied build-tree executable must resolve the runtime from inside the
    # bundle, never from an absolute path on the developer's Mac.  The legacy
    # launcher supplies DYLD_LIBRARY_PATH when running the unbundled binary.
    set_target_properties(ge PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    set(GOLDENEYE_MACOS_APP_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/dist/GoldenEye Metal.app")
    set(GOLDENEYE_MACOS_INFO_PLIST
        "${CMAKE_CURRENT_BINARY_DIR}/GoldenEyeMetal-Info.plist")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${GOLDENEYE_MACOS_INFO_PLIST}"
        @ONLY
    )

    # SPIRV-Cross is currently supplied by the host package manager rather
    # than the source tree.  A binary distribution must carry its license too.
    get_filename_component(_spirv_cross_prefix "${SPIRV_CROSS_INCLUDE_DIR}" DIRECTORY)
    find_file(GOLDENEYE_SPIRV_CROSS_LICENSE
        NAMES LICENSE LICENSE.txt LICENSE.md
        HINTS
            "${_spirv_cross_prefix}/opt/spirv-cross"
            "${_spirv_cross_prefix}/share/doc/spirv-cross"
        NO_DEFAULT_PATH
    )
    if(NOT GOLDENEYE_SPIRV_CROSS_LICENSE)
        message(FATAL_ERROR
            "Could not locate the SPIRV-Cross license beside the installed "
            "package; refusing to create an incomplete binary distribution")
    endif()

    # Always recreate the staged bundle. A stamp-based incremental target can
    # leave an accidentally added file in a release after the source that
    # created it has gone away.
    add_custom_target(goldeneye_macos_app
        COMMAND "${CMAKE_COMMAND}"
            "-DAPP_BUNDLE=${GOLDENEYE_MACOS_APP_DIR}"
            "-DGAME_EXECUTABLE=$<TARGET_FILE:ge>"
            "-DRUNTIME_LIBRARY=$<TARGET_FILE:rexruntime>"
            "-DINFO_PLIST=${GOLDENEYE_MACOS_INFO_PLIST}"
            "-DAPP_ICON=${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/GoldenEyeMetal.icns"
            "-DROOT_LICENSE=${REXSDK_DIR}/LICENSE"
            "-DROOT_NOTICES=${REXSDK_DIR}/THIRD_PARTY_NOTICES.md"
            "-DGAME_LICENSE=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "-DGAME_NOTICES=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "-DTHIRDPARTY_DIR=${REXSDK_DIR}/thirdparty"
            "-DSPIRV_CROSS_LICENSE=${GOLDENEYE_SPIRV_CROSS_LICENSE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/stage_app.cmake"
        DEPENDS
            ge
            rexruntime
            "${GOLDENEYE_MACOS_INFO_PLIST}"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/GoldenEyeMetal.icns"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/stage_app.cmake"
            "${REXSDK_DIR}/LICENSE"
            "${REXSDK_DIR}/THIRD_PARTY_NOTICES.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${GOLDENEYE_SPIRV_CROSS_LICENSE}"
        COMMENT "Staging unsigned GoldenEye Metal.app"
        VERBATIM
    )

    add_custom_target(goldeneye_macos_app_verify
        COMMAND "$<TARGET_FILE:goldeneye_crash_guard_test>"
        COMMAND "$<TARGET_FILE:goldeneye_player_stuck_telemetry_test>"
        COMMAND "$<TARGET_FILE:goldeneye_watchdog_test>"
        COMMAND "$<TARGET_FILE:goldeneye_host_pause_test>"
        COMMAND "$<TARGET_FILE:goldeneye_testing_tools_test>"
        COMMAND "$<TARGET_FILE:goldeneye_gpu_capture_test>"
        COMMAND "$<TARGET_FILE:goldeneye_launch_recovery_test>"
        COMMAND "$<TARGET_FILE:goldeneye_save_manager_test>"
        COMMAND "$<TARGET_FILE:metal_presenter_shader_test>"
        COMMAND "$<TARGET_FILE:metal_metalfx_scaler_test>"
        COMMAND "${CMAKE_COMMAND}"
            "-DGAME_EXECUTABLE=$<TARGET_FILE:ge>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/verify_pause_hook.cmake"
        COMMAND
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/verify_app.sh"
            "${GOLDENEYE_MACOS_APP_DIR}"
            "${GOLDENEYE_VERSION}"
            "${GOLDENEYE_BUILD_NUMBER}"
            "$<IF:$<BOOL:${REXGLUE_ENABLE_INPUT_TEST_HARNESS}>,on,off>"
        DEPENDS
            goldeneye_macos_app
            goldeneye_crash_guard_test
            goldeneye_crash_guard_codegen_verify
            goldeneye_player_stuck_telemetry_test
            goldeneye_watchdog_test
            goldeneye_host_pause_test
            goldeneye_testing_tools_test
            goldeneye_gpu_capture_test
            goldeneye_launch_recovery_test
            goldeneye_save_manager_test
            metal_metalfx_scaler_test
            metal_presenter_shader_test
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/verify_pause_hook.cmake"
            "${GOLDENEYE_RELEASE_MANIFEST}"
        COMMENT "Verifying unsigned GoldenEye Metal.app layout and linkage"
        VERBATIM
    )

    message(STATUS
        "GoldenEye Metal app bundle target: goldeneye_macos_app "
        "(${GOLDENEYE_MACOS_APP_DIR})")
endif()

# ---------------------------------------------------------------------------
# Headless GPU trace-dump oracle (graphics bring-up tooling, not the game).
#
# Replays a captured ReXGlue GPU trace (.xtr) through a backend with no window
# and writes the resulting guest output frame to disk (BMP + raw RGBA). The
# Vulkan backend is complete and its CaptureGuestOutput is implemented, so this
# produces the ground-truth reference image used to diff the native Metal
# backend during bring-up. It links the SDK runtime directly (NOT the
# recompiled game sources from rexglue_setup_target).
# ---------------------------------------------------------------------------
option(GOLDENEYE_BUILD_VULKAN_TRACE_DUMP
    "Build the source-tree-only Vulkan trace dump utility" OFF)
if(GOLDENEYE_BUILD_VULKAN_TRACE_DUMP)
    if(NOT REXGLUE_USE_VULKAN)
        message(FATAL_ERROR
            "GOLDENEYE_BUILD_VULKAN_TRACE_DUMP requires REXGLUE_USE_VULKAN=ON")
    endif()
    add_executable(trace_dump_vulkan
        ${REXSDK_DIR}/src/graphics/vulkan/trace_dump_main.cpp
    )
    target_link_libraries(trace_dump_vulkan PRIVATE rex::runtime)
    # Headers: rex::runtime is linked PRIVATE and doesn't re-export rexcore's
    # interface includes, so add them directly (mirrors rexglue_configure_target).
    if(TARGET rexcore)
        target_include_directories(trace_dump_vulkan PRIVATE
            $<TARGET_PROPERTY:rexcore,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    # Apply platform settings without injecting the windowed-app entry point;
    # this console tool provides its own main().
    rexglue_apply_target_settings(trace_dump_vulkan)
endif()

option(GOLDENEYE_BUILD_METAL_TRACE_DUMP
    "Build the source-tree-only native Metal trace dump utility" OFF)
if(GOLDENEYE_BUILD_METAL_TRACE_DUMP)
    if(NOT APPLE OR NOT REXGLUE_USE_METAL)
        message(FATAL_ERROR
            "GOLDENEYE_BUILD_METAL_TRACE_DUMP requires Apple Metal "
            "(APPLE=TRUE and REXGLUE_USE_METAL=ON)")
    endif()
    add_executable(trace_dump_metal
        ${REXSDK_DIR}/src/graphics/metal/trace_dump_main.mm
    )
    target_link_libraries(trace_dump_metal PRIVATE
        rex::runtime
        "-framework Foundation"
        "-framework Metal"
    )
    if(TARGET rexcore)
        target_include_directories(trace_dump_metal PRIVATE
            $<TARGET_PROPERTY:rexcore,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    rexglue_apply_target_settings(trace_dump_metal)
endif()
