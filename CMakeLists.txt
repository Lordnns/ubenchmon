cmake_minimum_required(VERSION 3.15)
project(ubenchmon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT WIN32)
    add_compile_options(-Wall -Wextra -march=native)
endif()

set(UBENCHMON_SOURCES
        src/utils.c
        src/setup.c
        src/monitor.c
        src/verify.c
)

set(UBENCHMON_PUBLIC_HEADER include/ubenchmon.h)

add_library(ubenchmon SHARED ${UBENCHMON_SOURCES})
target_include_directories(ubenchmon
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(ubenchmon PROPERTIES
        OUTPUT_NAME ubenchmon
        PUBLIC_HEADER "${UBENCHMON_PUBLIC_HEADER}"
)

add_library(ubenchmon_static STATIC ${UBENCHMON_SOURCES})
target_include_directories(ubenchmon_static
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
set_target_properties(ubenchmon_static PROPERTIES OUTPUT_NAME ubenchmon)

include(GNUInstallDirs)
install(TARGETS ubenchmon ubenchmon_static
        LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)