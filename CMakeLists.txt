cmake_minimum_required(VERSION 3.19)

set(VERSION 0.1.1)

project(hypridle
    DESCRIPTION "An idle management daemon for Hyprland"
    VERSION ${VERSION}
)

set(CMAKE_MESSAGE_LOG_LEVEL "STATUS")

if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DEBUG)
    message(STATUS "Configuring hypridle in Debug with CMake")
    add_compile_definitions(HYPRLAND_DEBUG)
else()
    add_compile_options(-O3)
    message(STATUS "Configuring hypridle in Release with CMake")
endif()

include_directories(
  .
  "protocols/"
)

# configure
set(CMAKE_CXX_STANDARD 23)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-value
	-Wno-missing-field-initializers -Wno-narrowing)

# dependencies
message(STATUS "Checking deps...")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET wayland-client wayland-protocols hyprlang>=0.4.0 sdbus-c++)

file(GLOB_RECURSE SRCFILES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(hypridle ${SRCFILES})
target_link_libraries(hypridle PRIVATE rt Threads::Threads PkgConfig::deps)

# protocols
find_program(WaylandScanner NAMES wayland-scanner)
message(STATUS "Found WaylandScanner at ${WaylandScanner}")
execute_process(
    COMMAND pkg-config --variable=pkgdatadir wayland-protocols
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE WAYLAND_PROTOCOLS_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")

function(protocol protoPath protoName external)
    if (external)
        execute_process(
            COMMAND ${WaylandScanner} client-header ${protoPath} protocols/${protoName}-protocol.h
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        execute_process(
            COMMAND ${WaylandScanner} private-code ${protoPath} protocols/${protoName}-protocol.c
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        target_sources(hypridle PRIVATE protocols/${protoName}-protocol.c)
    else()
        execute_process(
            COMMAND ${WaylandScanner} client-header ${WAYLAND_PROTOCOLS_DIR}/${protoPath} protocols/${protoName}-protocol.h
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        execute_process(
            COMMAND ${WaylandScanner} private-code ${WAYLAND_PROTOCOLS_DIR}/${protoPath} protocols/${protoName}-protocol.c
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        target_sources(hypridle PRIVATE protocols/${protoName}-protocol.c)
    endif()
endfunction()

make_directory(${CMAKE_SOURCE_DIR}/protocols) # we don't ship any custom ones so the dir won't be there
protocol("staging/ext-idle-notify/ext-idle-notify-v1.xml" "ext-idle-notify-v1" false)

# Installation
install(TARGETS hypridle)
