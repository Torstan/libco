cmake_minimum_required(VERSION 3.10)
project(libco LANGUAGES C CXX ASM)

set(CMAKE_MACOSX_RPATH 0)
set(LIBCO_VERSION 0.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBCO_COMMON_COMPILE_OPTIONS
        -g
        -fno-strict-aliasing
        -O2
        -Wall
        -Werror
        -pipe
        -fPIC)

set(LIBCO_PRIVATE_COMPILE_DEFINITIONS _REENTRANT)
set(LIBCO_PUBLIC_COMPILE_DEFINITIONS)

set(LIBCO_PLATFORM_LIBS pthread)
set(LIBCO_USE_UCONTEXT OFF)

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(LIBCO_USE_UCONTEXT ON)
        list(APPEND LIBCO_PUBLIC_COMPILE_DEFINITIONS
                _XOPEN_SOURCE
                USE_UCONTEXT)
        list(APPEND LIBCO_COMMON_COMPILE_OPTIONS
                -Wno-deprecated-declarations)
    endif()
else()
    list(APPEND LIBCO_PRIVATE_COMPILE_DEFINITIONS _GNU_SOURCE)
    list(APPEND LIBCO_COMMON_COMPILE_OPTIONS -m64)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -export-dynamic")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        list(APPEND LIBCO_PLATFORM_LIBS dl)
    endif()
endif()

set(LIBCO_SOURCE_FILES
        co_epoll.cpp
        co_cond.cpp
        thread_worker.cpp
        routine_context.cpp
        co_routine.cpp
        co_hook_sys_call.cpp)

if(NOT LIBCO_USE_UCONTEXT)
    list(APPEND LIBCO_SOURCE_FILES
            coctx.cpp
            coctx_swap.S)
endif()

add_library(colib_static STATIC ${LIBCO_SOURCE_FILES})
add_library(colib_shared SHARED ${LIBCO_SOURCE_FILES})

foreach(LIBCO_TARGET colib_static colib_shared)
    target_include_directories(${LIBCO_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${LIBCO_TARGET} PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    target_compile_definitions(${LIBCO_TARGET}
            PUBLIC ${LIBCO_PUBLIC_COMPILE_DEFINITIONS}
            PRIVATE ${LIBCO_PRIVATE_COMPILE_DEFINITIONS})
    set_target_properties(${LIBCO_TARGET} PROPERTIES
            OUTPUT_NAME colib
            CLEAN_DIRECT_OUTPUT 1)
endforeach()

set_target_properties(colib_shared PROPERTIES
        VERSION ${LIBCO_VERSION}
        SOVERSION ${LIBCO_VERSION})

macro(add_example_target EXAMPLE_TARGET)
    add_executable("${EXAMPLE_TARGET}" "example/${EXAMPLE_TARGET}.cpp")
    target_compile_options("${EXAMPLE_TARGET}" PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    target_compile_definitions("${EXAMPLE_TARGET}" PRIVATE ${LIBCO_PRIVATE_COMPILE_DEFINITIONS})
    target_link_libraries("${EXAMPLE_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
endmacro()

add_example_target(example_cond)
add_example_target(example_echocli)
add_example_target(example_echosvr)
add_example_target(example_poll)
add_example_target(example_setenv)
add_example_target(example_thread)

enable_testing()

macro(add_libco_test TEST_TARGET)
    add_executable("${TEST_TARGET}" "test/${TEST_TARGET}.cpp")
    target_compile_options("${TEST_TARGET}" PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    target_compile_definitions("${TEST_TARGET}" PRIVATE ${LIBCO_PRIVATE_COMPILE_DEFINITIONS})
    target_link_libraries("${TEST_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
    add_test(NAME "${TEST_TARGET}" COMMAND "${TEST_TARGET}")
endmacro()

add_libco_test(test_co_routine)
add_libco_test(test_co_async)
add_libco_test(test_co_poll)
add_libco_test(test_public_api)
