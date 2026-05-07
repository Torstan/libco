cmake_minimum_required(VERSION 2.8)
project(libco)

# This for mac osx only
set(CMAKE_MACOSX_RPATH 0)

# Set lib version
set(LIBCO_VERSION   0.5)

# Set cflags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-strict-aliasing --std=c++17 -O2 -Wall -Werror -pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -export-dynamic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-strict-aliasing -O2 -Wall -Werror -pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -m64")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -g")

# Use c, c++ and asm
enable_language(CXX C ASM)

set(LIBCO_PLATFORM_LIBS pthread)
if(NOT APPLE)
    list(APPEND LIBCO_PLATFORM_LIBS dl)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add source files
set(SOURCE_FILES
        co_epoll.cpp
        co_cond.cpp
        thread_worker.cpp
        routine_context.cpp
        co_routine.cpp
        co_hook_sys_call.cpp
        coctx.cpp
        coctx_swap.S)

# Add static and shared library target
add_library(colib_static STATIC ${SOURCE_FILES})
add_library(colib_shared SHARED ${SOURCE_FILES})

# Set library output name
set_target_properties(colib_static PROPERTIES OUTPUT_NAME colib)
set_target_properties(colib_shared PROPERTIES OUTPUT_NAME colib)

set_target_properties(colib_static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(colib_shared PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# Set shared library version, will generate libcolib.${LIBCO_VERSION}.so and a symbol link named libcolib.so
# For mac osx, the extension name will be .dylib
set_target_properties(colib_shared PROPERTIES VERSION ${LIBCO_VERSION} SOVERSION ${LIBCO_VERSION})



# Macro for add example target
macro(add_example_target EXAMPLE_TARGET)
    add_executable("${EXAMPLE_TARGET}" "example/${EXAMPLE_TARGET}.cpp")
    target_link_libraries("${EXAMPLE_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
endmacro(add_example_target)

add_example_target(example_cond)
add_example_target(example_echocli)
add_example_target(example_echosvr)
add_example_target(example_poll)
add_example_target(example_setenv)
add_example_target(example_thread)

enable_testing()

macro(add_libco_test TEST_TARGET)
    add_executable("${TEST_TARGET}" "test/${TEST_TARGET}.cpp")
    target_link_libraries("${TEST_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
    add_test(NAME "${TEST_TARGET}" COMMAND "${TEST_TARGET}")
endmacro(add_libco_test)

add_libco_test(test_co_routine)
add_libco_test(test_co_async)
add_libco_test(test_co_poll)
add_libco_test(test_public_api)
