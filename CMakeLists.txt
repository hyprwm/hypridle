cmake_minimum_required(VERSION 3.19)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} VERSION)

project(
  hypridle
  DESCRIPTION "An idle management daemon for Hyprland"
  VERSION ${VERSION})

set(CMAKE_MESSAGE_LOG_LEVEL "STATUS")

if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DEBUG)
  message(STATUS "Configuring hypridle in Debug with CMake")
  add_compile_definitions(HYPRLAND_DEBUG)
else()
  add_compile_options(-O3)
  message(STATUS "Configuring hypridle in Release with CMake")
endif()

add_compile_definitions(HYPRIDLE_VERSION="${VERSION}")

include_directories(. "protocols/")

include(GNUInstallDirs)

# configure
set(CMAKE_CXX_STANDARD 23)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-value
                    -Wno-missing-field-initializers -Wno-narrowing)
configure_file(systemd/hypridle.service.in systemd/hypridle.service @ONLY)

# dependencies
message(STATUS "Checking deps...")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(hyprwayland-scanner 0.4.4 REQUIRED)
pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  wayland-client
  wayland-protocols
  hyprlang>=0.4.0
  hyprutils>=0.2.0
  sdbus-c++>=0.2.0)

file(GLOB_RECURSE SRCFILES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(hypridle ${SRCFILES})
target_link_libraries(hypridle PRIVATE rt Threads::Threads PkgConfig::deps)

# protocols
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")
pkg_get_variable(WAYLAND_SCANNER_PKGDATA_DIR wayland-scanner pkgdatadir)
message(STATUS "Found wayland-scanner pkgdatadir at ${WAYLAND_SCANNER_PKGDATA_DIR}")

pkg_check_modules(hyprland_protocols_dep REQUIRED IMPORTED_TARGET hyprland-protocols>=0.6.0)
pkg_get_variable(HYPRLAND_PROTOCOLS hyprland-protocols pkgdatadir)
message(STATUS "Found hyprland-protocols at ${HYPRLAND_PROTOCOLS}")

function(protocolnew protoPath protoName external)
  if(external)
    set(path ${protoPath})
  else()
    set(path ${WAYLAND_PROTOCOLS_DIR}/${protoPath})
  endif()
  message(STATUS "Full proto path: ${path}")
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/${protoName}.cpp
           ${CMAKE_SOURCE_DIR}/protocols/${protoName}.hpp
    COMMAND hyprwayland-scanner --client ${path}/${protoName}.xml
            ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(hypridle PRIVATE protocols/${protoName}.cpp
                                   protocols/${protoName}.hpp)
endfunction()
function(protocolWayland)
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/wayland.cpp
           ${CMAKE_SOURCE_DIR}/protocols/wayland.hpp
    COMMAND hyprwayland-scanner --wayland-enums --client
            ${WAYLAND_SCANNER_PKGDATA_DIR}/wayland.xml ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(hypridle PRIVATE protocols/wayland.cpp protocols/wayland.hpp)
endfunction()

make_directory(${CMAKE_SOURCE_DIR}/protocols) # we don't ship any custom ones so

protocolwayland()

protocolnew("staging/ext-idle-notify" "ext-idle-notify-v1" false)
protocolnew("${HYPRLAND_PROTOCOLS}/protocols" "hyprland-lock-notify-v1" true)

# Installation
install(TARGETS hypridle)
install(FILES ${CMAKE_BINARY_DIR}/systemd/hypridle.service
        DESTINATION "lib/systemd/user")

install(
  FILES ${CMAKE_SOURCE_DIR}/assets/example.conf
  DESTINATION ${CMAKE_INSTALL_FULL_DATAROOTDIR}/hypr
  RENAME hypridle.conf)
