# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in the FetchContent module
include(FetchContent)

# Declare the FreeRTOS-Kernel repository and force recursive submodules
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY         https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG                V11.3.0
    GIT_SUBMODULES_RECURSE TRUE
)

# Populate the content (download it)
FetchContent_GetProperties(freertos_kernel)
if(NOT freertos_kernel_POPULATED)
    FetchContent_Populate(freertos_kernel)
endif()

# Set the path variable for the SDK import script
set(FREERTOS_KERNEL_PATH ${freertos_kernel_SOURCE_DIR})

# Pull in FreeRTOS
include(FreeRTOS_Kernel_import.cmake)

project(pico_zstack_coordinator_poc C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(pico_zstack_coordinator_poc 
        main.c
        ble_server.c
)

target_compile_definitions(pico_zstack_coordinator_poc PRIVATE
    configNUMBER_OF_CORES=1
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)

pico_set_program_name(pico_zstack_coordinator_poc "pico_zstack_coordinator_poc")
pico_set_program_version(pico_zstack_coordinator_poc "0.1")

# Process .gatt file into a C header
pico_btstack_make_gatt_header(pico_zstack_coordinator_poc PRIVATE "${CMAKE_CURRENT_LIST_DIR}/zigbee_bridge.gatt")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico_zstack_coordinator_poc 1)
pico_enable_stdio_usb(pico_zstack_coordinator_poc 0)

# Add the standard library to the build
target_link_libraries(pico_zstack_coordinator_poc
        PRIVATE
        pico_stdlib
        tinyusb_host
        tinyusb_device
        pico_cyw43_arch_none
        pico_async_context_freertos
        FreeRTOS-Kernel-Heap4
        pico_btstack_ble
        pico_btstack_cyw43
)

# Add the standard include files to the build
target_include_directories(pico_zstack_coordinator_poc PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(pico_zstack_coordinator_poc 
        
        )

pico_add_extra_outputs(pico_zstack_coordinator_poc)

