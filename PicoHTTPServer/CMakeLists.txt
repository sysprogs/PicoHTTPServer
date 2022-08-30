cmake_minimum_required(VERSION 3.12)
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(PicoHTTPServer C CXX ASM)
include(PicoSDKConfig.cmake)
add_subdirectory("${PICO_SDK_PATH}/FreeRTOS/portable/ThirdParty/GCC/RP2040" FREERTOS_KERNEL)
pico_sdk_init()

if(DEFINED SYSPROGS_FRAMEWORKS_FILE)
include(${SYSPROGS_FRAMEWORKS_FILE})
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_executable(PicoHTTPServer
        main.c
        dhcpserver/dhcpserver.c
        dns/dnsserver.c
        httpserver.c
        server_settings.c)
target_compile_definitions(PicoHTTPServer PRIVATE
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        NO_SYS=0)
target_include_directories(PicoHTTPServer PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../..)
target_link_libraries(PicoHTTPServer
        pico_cyw43_arch_lwip_sys_freertos
        pico_stdlib
        pico_lwip_iperf
        FreeRTOS-Kernel-Heap4
        Profiler)
pico_add_extra_outputs(PicoHTTPServer)
