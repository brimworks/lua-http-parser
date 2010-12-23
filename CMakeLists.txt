cmake_minimum_required(VERSION 2.8)

project(lua-http-parser C)

set(BUILD_SHARED_LIBS TRUE)
set(CMAKE_SHARED_MODULE_PREFIX "")

include(FindPkgConfig)

#
# Find system libraries
#
set(COMMON_CFLAGS)

## Lua 5.1.x
pkg_search_module(LUA REQUIRED lua lua5.1 lua-5.1)
set(COMMON_CFLAGS "${COMMON_CFLAGS} ${LUA_CFLAGS}")

## setup git submodules
execute_process(COMMAND git submodule init)
execute_process(COMMAND git submodule update)

include_directories(
				${CMAKE_CURRENT_SOURCE_DIR}
				${CMAKE_CURRENT_SOURCE_DIR}/http-parser
				)
add_library(parser MODULE lua-http-parser.c http-parser/http_parser.c)
set_target_properties(parser PROPERTIES COMPILE_FLAGS "${COMMON_CFLAGS}")
install(TARGETS parser
				LIBRARY DESTINATION ${INSTALL_LIBDIR}
				)

