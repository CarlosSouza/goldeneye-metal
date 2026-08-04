# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput OBJECT
    input_system.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Device gyro for gyro aim with grip controllers (see ios_device_motion.h).
    target_sources(rexinput PRIVATE ios_device_motion.mm)
    target_link_libraries(rexinput PUBLIC "-framework CoreMotion")
endif()
if(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
    target_sources(rexinput PRIVATE sdl/sdl_virtual_gamepad_harness.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore rexui SDL3::SDL3)
