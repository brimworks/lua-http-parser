cmake_minimum_required(VERSION 2.8)

project(lua-http-parser C)

set(BUILD_SHARED_LIBS TRUE)

set(INSTALL_CMOD share/lua/cmod CACHE PATH "Directory to install Lua binary modules (configure lua via LUA_CPATH)")

## Lua 5.1.x
include(FindLua51)
if(!${LUA51_FOUND})
    message(FATAL_ERROR "The FindLua51 module could not find lua :-(")
endif()


## setup git submodules
execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMAND git submodule init)
execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMAND git submodule update)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}
                    ${CMAKE_CURRENT_SOURCE_DIR}/http-parser
                    ${LUA_INCLUDE_DIR})

add_library(parser MODULE lua-http-parser.c http-parser/http_parser.c)
set_target_properties(parser PROPERTIES PREFIX "")
set_target_properties(parser PROPERTIES COMPILE_FLAGS "${CFLAGS}")
set_target_properties(parser PROPERTIES OUTPUT_NAME parser)

install(TARGETS parser
        DESTINATION "${INSTALL_CMOD}/http")


## Setup test stuff
include(CTest)
add_test(test ${LUA} ${CMAKE_CURRENT_SOURCE_DIR}/test.lua ${CMAKE_CURRENT_SOURCE_DIR}/ ${CMAKE_CURRENT_BINARY_DIR}/)
#add_test(benchmark ${LUA} ${CMAKE_CURRENT_SOURCE_DIR}/benchmark.lua ${CMAKE_CURRENT_SOURCE_DIR}/ ${CMAKE_CURRENT_BINARY_DIR}/)
set_tests_properties(test
                     PROPERTIES
                     FAIL_REGULAR_EXPRESSION
                     "not ok")
