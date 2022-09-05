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

if (CMAKE_HOST_WIN32)
    set(SIMPLE_FS_BUILDER_EXE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/SimpleFSBuilder/SimpleFSBuilder.exe)
else()
    set(SIMPLE_FS_BUILDER_EXE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/SimpleFSBuilder/build/SimpleFSBuilder)
endif()

if (NOT EXISTS ${SIMPLE_FS_BUILDER_EXE})
    message(FATAL_ERROR "Missing ${SIMPLE_FS_BUILDER_EXE}. Please build it before building this project.")
endif()

function(add_resource_folder target name path)
	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.fs ${CMAKE_CURRENT_BINARY_DIR}/__rerun_${name}.fs 
		COMMAND ${SIMPLE_FS_BUILDER_EXE}
		ARGS ${path} ${CMAKE_CURRENT_BINARY_DIR}/${name}.fs
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} 
		COMMENT "Generating ${name}.fs")

	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.o0
		COMMAND ${CMAKE_LINKER}
		ARGS -r -b binary ${name}.fs -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.o0
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.fs ${CMAKE_CURRENT_BINARY_DIR}/__rerun_${name}.fs 
		COMMENT "Wrapping ${name}.fs")

	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.o
		COMMAND ${CMAKE_OBJCOPY} 
		ARGS --rename-section .data=.rodata
			 ${CMAKE_CURRENT_BINARY_DIR}/${name}.o0 ${CMAKE_CURRENT_BINARY_DIR}/${name}.o
		DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.o0
		COMMENT "Renaming ${name}")

    add_custom_target(${name}-fs DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.o)
    add_dependencies(${target} ${name}-fs)
    target_link_libraries(${target} ${CMAKE_CURRENT_BINARY_DIR}/${name}.o)

endfunction()

add_executable(PicoHTTPServer
        main.c
        dhcpserver/dhcpserver.c
        dns/dnsserver.c
        httpserver.c
        server_settings.c)

add_resource_folder(PicoHTTPServer www www)

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
        FreeRTOS-Kernel-Heap4)

if (TARGET Profiler)
    target_link_libraries(PicoHTTPServer 
            Profiler)
endif()

pico_add_extra_outputs(PicoHTTPServer)
