cmake_minimum_required(VERSION 2.8.11)
cmake_policy(SET CMP0003 NEW)
project(quicrq C CXX)
find_package (Threads REQUIRED)

set(CMAKE_C_STANDARD 11)

set(CMAKE_C_FLAGS "-std=c99 -Wall -Werror -O2 -g ${CC_WARNING_FLAGS} ${CMAKE_C_FLAGS}")

if(DISABLE_DEBUG_PRINTF)
    set(CMAKE_C_FLAGS "-DDISABLE_DEBUG_PRINTF ${CMAKE_C_FLAGS}")
endif()

set(QUICRQ_LIBRARY_FILES
    lib/quicrq.c
    lib/segment.c
)

set(QUICRQ_TEST_LIBRARY_FILES
    test/basic_test.c
    test/network_test.c
    test/test_media.c
)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Picoquic REQUIRED)
message(STATUS "Picoquic/include: ${Picoquic_INCLUDE_DIRS}" )
message(STATUS "Picoquic library: ${Picoquic_LIBRARIES}" )

find_package(PTLS REQUIRED)
message(STATUS "picotls/include: ${PTLS_INCLUDE_DIRS}" )
message(STATUS "picotls libraries: ${PTLS_LIBRARIES}" )

find_package(OpenSSL REQUIRED)
message(STATUS "root: ${OPENSSL_ROOT_DIR}")
message(STATUS "OpenSSL_VERSION: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL_INCLUDE_DIR: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL_LIBRARIES: ${OPENSSL_LIBRARIES}")

include_directories(include lib test 
    ${Picoquic_INCLUDE_DIRS} ${PTLS_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})

add_library(quicrq-core
    ${quicrq_LIBRARY_FILES}
)

if(FALSE) # fake a block comment to comment out all applications for now
add_executable(quicrq_app
    quicrq_cli_app/quicrq_app.c
)

target_link_libraries(quicrq_app
    quicrq-core
    ${Picoquic_LIBRARIES}
    ${PTLS_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
)

set(TEST_EXES quicrq_t)

add_executable(quicrq_t
    src/quicrq_t.c
    ${quicrq_TEST_LIBRARY_FILES}
)

target_link_libraries(quicrq_t
    quicrq-core
    ${Picoquic_LIBRARIES}
    ${PTLS_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
)

set(TEST_EXES quicrq_t)
endif()

# get all project files for formatting
file(GLOB_RECURSE CLANG_FORMAT_SOURCE_FILES *.c *.h)

# Adds clangformat as target that formats all source files
add_custom_target(
    clangformat
    COMMAND clang-format
    -style=Webkit
    -i
    ${CLANG_FORMAT_SOURCE_FILES}
)