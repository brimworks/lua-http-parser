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

/* The index which contains the userdata fenv */
#define FENV_IDX 3

static int lhp_http_cb(http_parser* parser, const char* name) {
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);
    assert(lua_checkstack(L, 2));

    lua_getfield(L, FENV_IDX, name);
    if ( lua_isfunction(L, -1) ) {
        lua_pushboolean(L, 0);
    } else {
        lua_pop(L, 1);
    }

    return 0;
}

static int lhp_http_data_cb(http_parser* parser, const char* name, const char* str, size_t len) {
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);
    assert(lua_checkstack(L, 2));

    lua_getfield(L, FENV_IDX, name);
    if ( lua_isfunction(L, -1) ) {
        if ( lua_rawequal(L, -3, -1) ) {
            /* Re-use this event */
            lua_pop(L, 1);
            assert(lua_istable(L, -1));
            lua_pushlstring(L, str, len);
            lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
        } else {
            lua_createtable(L, 1, 0);
            lua_pushlstring(L, str, len);
            lua_rawseti(L, -2, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    return 0;
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
    assert(NULL != parser);

    /* Copy functions to new fenv table */
    lua_newtable(L);
    lua_pushnil(L);
    while ( lua_next(L, 1) != 0 ) {
        if ( lua_isstring(L, -2) && lua_isfunction(L, -1) ) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            /* <fenv>, <key>, <key>, <value> */
            lua_rawset(L, -4);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_setfenv(L, -2);

    /* Get the metatable: */
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
    size_t       result;
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

    lua_settop(L, 2);
    lua_getfenv(L, 1);

    lua_rawgeti(L, FENV_IDX, 1);
    if ( lua_isfunction(L, -1) ) {
        lua_rawgeti(L, FENV_IDX, 2);
    } else {
        lua_pop(L, 1);
    }

    parser->data = L;
    result = http_parser_execute(parser, &settings, str, len);

    if ( lua_istable(L, -1) && 0 != len && lua_gettop(L) > FENV_IDX ) {
        /* Save this event for the next time execute is ran */
        lua_rawseti(L, FENV_IDX, 2);
        lua_rawseti(L, FENV_IDX, 1);
    } else {
        lua_pushnil(L);
        lua_rawseti(L, FENV_IDX, 1);
    }

    /* Transform the stack into a table: */
    len = lua_gettop(L) - FENV_IDX;
    lua_createtable(L, len, 0);
    lua_insert(L, FENV_IDX);
    for (; len > 0; --len ) {
        lua_rawseti(L, FENV_IDX, len);
    }
    lua_pop(L, 1);
    lua_pushnumber(L, result);

    return 2;
}

static int lhp_should_keep_alive(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    lua_pushboolean(L, http_should_keep_alive(parser));
    return 1;
}

static int lhp_is_upgrade(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    lua_pushboolean(L, parser->upgrade);
    return 1;
}

static int lhp_method(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    switch(parser->method) {
    case HTTP_DELETE:    lua_pushliteral(L, "DELETE"); break;
    case HTTP_GET:       lua_pushliteral(L, "GET"); break;
    case HTTP_HEAD:      lua_pushliteral(L, "HEAD"); break;
    case HTTP_POST:      lua_pushliteral(L, "POST"); break;
    case HTTP_PUT:       lua_pushliteral(L, "PUT"); break;
    case HTTP_CONNECT:   lua_pushliteral(L, "CONNECT"); break;
    case HTTP_OPTIONS:   lua_pushliteral(L, "OPTIONS"); break;
    case HTTP_TRACE:     lua_pushliteral(L, "TRACE"); break;
    case HTTP_COPY:      lua_pushliteral(L, "COPY"); break;
    case HTTP_LOCK:      lua_pushliteral(L, "LOCK"); break;
    case HTTP_MKCOL:     lua_pushliteral(L, "MKCOL"); break;
    case HTTP_MOVE:      lua_pushliteral(L, "MOVE"); break;
    case HTTP_PROPFIND:  lua_pushliteral(L, "PROPFIND"); break;
    case HTTP_PROPPATCH: lua_pushliteral(L, "PROPPATCH"); break;
    case HTTP_UNLOCK:    lua_pushliteral(L, "UNLOCK"); break;
    default:
        lua_pushnumber(L, parser->method);
    }
    return 1;
}

static int lhp_status_code(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    lua_pushnumber(L, parser->status_code);
    return 1;
}

/* The execute method has a "lua based stub" so that callbacks
 * can yield without having to apply the CoCo patch to Lua. */
static const char* lhp_execute_lua =
    "return function(...)\n"
    "  local callbacks, result = execute(...)\n"
    "  for i = 1, #callbacks, 2 do\n"
    "    if callbacks[i+1] then\n"
    "      callbacks[i](concat(callbacks[i+1]))\n"
    "    else\n"
    "      callbacks[i]()\n"
    "    end\n"
    "  end\n"
    "  return result\n"
    "end";
static void lhp_push_execute_fn(lua_State* L) {
    int top = lua_gettop(L);
    int ok  = luaL_loadstring(L, lhp_execute_lua);
    assert(0 == ok);

    /* Create environment table: */
    lua_createtable(L, 0, 3);

    lua_pushcfunction(L, lhp_execute);
    lua_setfield(L, -2, "execute");

    lua_getfield(L, LUA_GLOBALSINDEX, "require");
    lua_pushliteral(L, "table");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "concat");
    lua_setfield(L, -3, "concat");
    lua_pop(L, 1);

    ok = lua_setfenv(L, -2);
    assert(ok);
    lua_call(L, 0, 1);

    /* Compiled lua function should be at the top of the stack now. */
    assert(lua_gettop(L) == top + 1);
    assert(lua_isfunction(L, -1));
}

LUALIB_API int luaopen_http_parser(lua_State* L) {
    /* parser metatable init */
    luaL_newmetatable(L, PARSER_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lhp_is_upgrade);
    lua_setfield(L, -2, "is_upgrade");

    lua_pushcfunction(L, lhp_method);
    lua_setfield(L, -2, "method");

    lua_pushcfunction(L, lhp_status_code);
    lua_setfield(L, -2, "status_code");

    lua_pushcfunction(L, lhp_should_keep_alive);
    lua_setfield(L, -2, "should_keep_alive");

    lhp_push_execute_fn(L);
    lua_setfield(L, -2, "execute");

    lua_pop(L, 1);

    /* export http.parser */
    lua_newtable(L);

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
