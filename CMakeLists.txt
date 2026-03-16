cmake_minimum_required(VERSION 3.14)
project(libsdi12-verifier VERSION 0.6.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── libsdi12: system library (preferred) or git submodule fallback ────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSDI12 QUIET IMPORTED_TARGET sdi12)
endif()

if(TARGET PkgConfig::LIBSDI12)
    message(STATUS "libsdi12: using system library (${LIBSDI12_VERSION})")
else()
    message(STATUS "libsdi12: system library not found, using submodule")
    set(LIBSDI12_DIR ${CMAKE_SOURCE_DIR}/lib/libsdi12)
    if(NOT EXISTS ${LIBSDI12_DIR}/sdi12.h)
        message(FATAL_ERROR
            "libsdi12 submodule not initialised.\n"
            "Run: git submodule update --init")
    endif()
    add_library(sdi12 STATIC
        ${LIBSDI12_DIR}/sdi12_crc.c
        ${LIBSDI12_DIR}/sdi12_sensor.c
        ${LIBSDI12_DIR}/sdi12_master.c
    )
    target_include_directories(sdi12 PUBLIC ${LIBSDI12_DIR})
endif()

# ── Platform HAL ──────────────────────────────────────────────────────
if(WIN32)
    set(HAL_SOURCES hal/hal_win32.c)
else()
    set(HAL_SOURCES hal/hal_posix.c)
endif()

# Loopback HAL is always compiled (used by --self-test)
list(APPEND HAL_SOURCES hal/hal_loopback.c)

# ── Verifier executable ──────────────────────────────────────────────
add_executable(sdi12-verifier
    src/main.c
    src/timing.c
    src/test_suite.c
    src/sensor_tests.c
    src/recorder_tests.c
    src/monitor.c
    src/reporter.c
    src/transparent.c
    ${HAL_SOURCES}
)

target_include_directories(sdi12-verifier PRIVATE
    include
)

if(TARGET PkgConfig::LIBSDI12)
    target_link_libraries(sdi12-verifier PRIVATE PkgConfig::LIBSDI12)
else()
    target_link_libraries(sdi12-verifier PRIVATE sdi12)
endif()

# Platform-specific link libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(sdi12-verifier PRIVATE rt)
endif()

# ── Compiler warnings ────────────────────────────────────────────────
if(MSVC)
    target_compile_options(sdi12-verifier PRIVATE /W4)
else()
    target_compile_options(sdi12-verifier PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Install ──────────────────────────────────────────────────────────
install(TARGETS sdi12-verifier RUNTIME DESTINATION bin)
