cmake_minimum_required(VERSION 3.14)
project(libsdi12-verifier VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── libsdi12 (git submodule) ──────────────────────────────────────────
set(LIBSDI12_DIR ${CMAKE_SOURCE_DIR}/lib/libsdi12)

add_library(sdi12 STATIC
    ${LIBSDI12_DIR}/sdi12_crc.c
    ${LIBSDI12_DIR}/sdi12_sensor.c
    ${LIBSDI12_DIR}/sdi12_master.c
)
target_include_directories(sdi12 PUBLIC ${LIBSDI12_DIR})

# ── Platform HAL ──────────────────────────────────────────────────────
if(WIN32)
    set(HAL_SOURCES hal/hal_win32.c)
else()
    set(HAL_SOURCES hal/hal_posix.c)
endif()

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
    ${LIBSDI12_DIR}
)

target_link_libraries(sdi12-verifier PRIVATE sdi12)

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
