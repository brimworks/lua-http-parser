#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include "http-parser/http_parser.h"

#define PARSER_MT "http.parser{parser}"

#define check_parser(L, narg)                                   \
    ((http_parser*)luaL_checkudata((L), (narg), PARSER_MT))

#define abs_index(L, i)                    \
    ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? \
     (i) : lua_gettop(L) + (i) + 1)

static char* obj_registry = "http.parser{obj}";

static void push_registry(lua_State* L) {
    lua_pushlightuserdata(L, &obj_registry);
    lua_rawget(L, LUA_REGISTRYINDEX);
    assert(lua_istable(L, -1) /* luaopen_http_parser() should create this */);
}

static lua_State* push_parser(http_parser* parser) {
    assert(parser != NULL);
    assert(parser->data != NULL);
    lua_State* L = (lua_State*)parser->data;

    lua_checkstack(L, 10);

    push_registry(L);
    lua_pushlightuserdata(L, parser);
    lua_rawget(L, -2);
    lua_remove(L, -2);
}

static int lhp_http_cb(http_parser* parser, const char* name) {
    int result = 0;
    lua_State* L = push_parser(parser);

    lua_getfenv(L, -1);
    lua_getfield(L, -1, name);
    if ( lua_isfunction(L, -1) ) {
        lua_pushvalue(L, -3);
        lua_call(L, 1, 1);
        if ( lua_isnumber(L, -1) ) result = lua_tointeger(L, -1);
        lua_pop(L, 2);
    } else {
        lua_pop(L, 3);
    }

    return result;
}

static int lhp_http_data_cb(http_parser* parser, const char* name, const char* str, size_t len) {
    int result = 0;
    lua_State* L = push_parser(parser);

    lua_getfenv(L, -1);
    lua_getfield(L, -1, name);
    if ( lua_isfunction(L, -1) ) {
        lua_pushvalue(L, -3);
        lua_pushlstring(L, str, len);
        lua_call(L, 2, 1);
        if ( lua_isnumber(L, -1) ) result = lua_tointeger(L, -1);
        lua_pop(L, 2);
    } else {
        lua_pop(L, 3);
    }

    return result;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_message_begin");
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_path", str, len);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_query_string", str, len);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_url", str, len);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_fragment", str, len);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_header_field", str, len);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_header_value", str, len);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_headers_complete");
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_body", str, len);
}

static int lhp_message_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_message_complete");
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    luaL_checktype(L, 1, LUA_TTABLE);

    http_parser* parser = (http_parser*)lua_newuserdata(L, sizeof(http_parser));
    assert(parser);

    lua_pushvalue(L, 1);
    lua_setfenv(L, -2);

    luaL_getmetatable(L, PARSER_MT);
    assert(!lua_isnil(L, -1)/* PARSER_MT found? */);

    http_parser_init(parser, type);
    parser->data = L;

    lua_setmetatable(L, -2);

    return 1;
}

static int lhp_request(lua_State* L) {
    return lhp_init(L, HTTP_REQUEST);
}
static int lhp_response(lua_State* L) {
    return lhp_init(L, HTTP_RESPONSE);
}

static int lhp_execute(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    size_t       len;
    const char*  str = luaL_checklstring(L, 2, &len);

    static http_parser_settings settings = {
        .on_message_begin    = lhp_message_begin_cb,
        .on_path             = lhp_path_cb,
        .on_query_string     = lhp_query_string_cb,
        .on_url              = lhp_url_cb,
        .on_fragment         = lhp_fragment_cb,
        .on_header_field     = lhp_header_field_cb,
        .on_header_value     = lhp_header_value_cb,
        .on_headers_complete = lhp_headers_complete_cb,
        .on_body             = lhp_body_cb,
        .on_message_complete = lhp_message_complete_cb
    };

    size_t result = http_parser_execute(parser, settings, str, len);

    lua_pushnumber(L, result);

    return 1;
}

static int lhp_should_keep_alive(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    lua_pushboolean(L, http_should_keep_alive(parser));
    return 1;
}

LUALIB_API int luaopen_http_parser(lua_State* L) {
    /* parser metatable init */
    luaL_newmetatable(L, PARSER_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lhp_should_keep_alive);
    lua_setfield(L, -2, "should_keep_alive");

    lua_pushcfunction(L, lhp_execute);
    lua_setfield(L, -2, "execute");

    lua_pop(L, 1);

    /* "object registry" init */
    lua_pushlightuserdata(L, &obj_registry);
    lua_newtable(L);

    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");

    lua_pushvalue(L, -1);
    lua_setmetatable(L, -2);

    lua_rawset(L, LUA_REGISTRYINDEX);

    /* export http.parser */
    lua_newtable(L);

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
