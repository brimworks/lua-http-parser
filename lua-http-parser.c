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

/* The indices into the Lua stack for various pieces of information */
#define ST_FENV_IDX        3
#define ST_TODO_IDX        4
#define ST_BUFFER_IDX      5

/* The FENV contains string keys for each callback, it also contains
 * the following numeric keys:
 */
#define FENV_BUFFER_IDX      1

/* The set of potentially buffered callbacks.
 */
enum {
    bitset_url              = 0x001,
    bitset_path             = 0x002,
    bitset_query_string     = 0x004,
    bitset_fragment         = 0x008,
    bitset_header_field     = 0x010,
    bitset_header_value     = 0x020,
    bitset_body             = 0x040,
};

/* How man elements are in the buffer?
 */
#define BITSET_LEN 7

/* Map a bitset element into the index used to store buffered output.
 */
static int bitset_to_idx(int bitset_elm) {
    switch ( bitset_elm ) {
    case bitset_url:          return 1;
    case bitset_path:         return 2;
    case bitset_query_string: return 3;
    case bitset_fragment:     return 4;
    case bitset_header_field: return 5;
    case bitset_header_value: return 6;
    case bitset_body:         return 7;
    }
    assert(!"Reachable");
    return 0;
}

/* Map a bitset element into the name of the callback.
 */
static const char* bitset_to_name(int bitset_elm) {
    switch ( bitset_elm ) {
    case bitset_url:          return "on_url";
    case bitset_path:         return "on_path";
    case bitset_query_string: return "on_query_string";
    case bitset_fragment:     return "on_fragment";
    case bitset_header_field: return "on_header_field";
    case bitset_header_value: return "on_header_value";
    case bitset_body:         return "on_body";
    }
    assert(!"Reachable");
    return 0;
}

/* Map the index of the buffered output back to a bitset element.
 */
static int idx_to_bitset(int idx) {
    switch ( idx ) {
    case 1: return bitset_url;
    case 2: return bitset_path;
    case 3: return bitset_query_string;
    case 4: return bitset_fragment;
    case 5: return bitset_header_field;
    case 6: return bitset_header_value;
    case 7: return bitset_body;
    }
    assert(!"Reachable");
    return 0;
}

/* Replaces a table on the top of the stack with a string that
 * concatinates all elements of the numeric keyed elements from the
 * table in order.
 */
static void table_concat(lua_State* L) {
    luaL_Buffer buff;
    int         cur;
    int         tbl = lua_gettop(L);
    size_t      len = lua_objlen(L, tbl);

    luaL_buffinit(L, &buff);
    for ( cur=1; cur <= len; cur++ ) {
        lua_rawgeti(L, tbl, cur);
        luaL_addvalue(&buff);
    }
    luaL_pushresult(&buff);
    lua_replace(L, -2);
}

/* Move everything out of the buffer table and into the todo table
 * EXCEPT the specified except_fns (except_fns is a bit set).
 */
static void flush_buffer(lua_State* L, int except_bitset) {
    int remove_bitset = 0;
    int mask;

    lua_pushnil(L);
    while ( lua_next(L, ST_BUFFER_IDX) != 0 ) {
        int idx        = lua_tointeger(L, -2);
        int bitset_elm = idx_to_bitset(idx);

        if ( ! ( bitset_elm & except_bitset ) ) {
            /* Flush it */
            lua_getfield(L, ST_FENV_IDX, bitset_to_name(bitset_elm));
            if ( lua_isfunction(L, -1) ) {
                lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
                assert(lua_istable(L, -1));
                table_concat(L);
                lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
            } else {
                lua_pop(L, 2);
            }
            remove_bitset |= bitset_elm;
        } else {
            lua_pop(L, 1);
        }
    }

    for ( mask=remove_bitset & -remove_bitset; remove_bitset; mask <<= 1 ) {
        if ( ! ( remove_bitset & mask ) ) continue;
        remove_bitset &= ~mask;
        lua_pushnil(L);
        lua_rawseti(L, ST_BUFFER_IDX, bitset_to_idx(mask));
    }
}

/* Post Condition: The BUFFER table flushed to TODO and if a callback
 * exists for 'name' in FENV, then the pair <func>, <false> is pushed
 * into the TODO table.
 */
static int lhp_http_cb(http_parser* parser, const char* name) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    flush_buffer(L, 0);

    lua_getfield(L, ST_FENV_IDX, name);
    if ( ! lua_isfunction(L, -1) ) {
        lua_pop(L, 1);
    } else {
        lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
        lua_pushboolean(L, 0);
        lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
    }

    return 0;
}

/* Post Condition: If a callback exists in FENV for 'name', then the
 * BUFFER table has <func> key's table appended with str (and if a
 * table doesn't exist, a new one is created).  Also, any buffered
 * callbacks are flushed except for the callbacks in the except_fns
 * bitset.
 */
static int lhp_http_data_cb(http_parser* parser, int bitset_cb, const char* str, size_t len, int except_bitset) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    flush_buffer(L, except_bitset);

    if ( ! except_bitset ) {
        /* Bypass the "buffer" since nothing is getting buffered */
        lua_getfield(L, ST_FENV_IDX, bitset_to_name(bitset_cb));
        if ( ! lua_isfunction(L, -1) ) {
            lua_pop(L, 1);
        } else {
            lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
            lua_pushlstring(L, str, len);
            lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
        }
    } else {
        lua_getfield(L, ST_FENV_IDX, bitset_to_name(bitset_cb));
        if ( ! lua_isfunction(L, -1) ) {
            lua_pop(L, 1);
        } else {
            int idx = bitset_to_idx(bitset_cb);
            lua_rawgeti(L, ST_BUFFER_IDX, idx);
            if ( ! lua_istable(L, -1) ) {
                lua_pop(L, 1);
                lua_createtable(L, 1, 0);
                lua_pushvalue(L, -1);
                lua_rawseti(L, ST_BUFFER_IDX, idx);
            }
            lua_pushlstring(L, str, len);
            lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
            lua_pop(L, 2);
        }
    }
    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_message_begin");
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_url, str, len, bitset_url | bitset_path | bitset_query_string | bitset_fragment);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_path, str, len, bitset_url | bitset_path);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_query_string, str, len, bitset_url | bitset_query_string);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_fragment, str, len, bitset_url | bitset_fragment);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_header_field, str, len, bitset_header_field);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, bitset_header_value, str, len, bitset_header_value);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_headers_complete");
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    int result;
    result = lhp_http_data_cb(parser, bitset_body, str, len, 0);
    return result;
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
        /* ..., <tbl fenv>, <key>, <value> */
        if ( lua_isstring(L, -2) && lua_isfunction(L, -1) ) {
            /* Set tbl fenv */
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -5);
        }
        lua_pop(L, 1);
    }

    /* Save the buffer table into the fenv */
    lua_createtable(L, 0, 3);
    lua_rawseti(L, -2, FENV_BUFFER_IDX);

    /* Save the fenv into the user data */
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

/* Pre-Condition: The Lua stack contains <http_parser>, <str> where
 * <str> is a string to be feed into the parser.
 *
 * During parser execution the Lua stack will contain:
 *
 * <http_parser>, <str>, <fenv>, <buffer-tbl>, <todo-tbl>
 *
 */
static int lhp_execute(lua_State* L) {
    http_parser* parser = check_parser(L, 1);
    size_t       len;
    size_t       result;

    const char*  str = luaL_checklstring(L, 2, &len);

    static http_parser_settings settings = {
        .on_message_begin    = lhp_message_begin_cb,
        .on_url              = lhp_url_cb,
        .on_path             = lhp_path_cb,
        .on_query_string     = lhp_query_string_cb,
        .on_fragment         = lhp_fragment_cb,
        .on_header_field     = lhp_header_field_cb,
        .on_header_value     = lhp_header_value_cb,
        .on_headers_complete = lhp_headers_complete_cb,
        .on_body             = lhp_body_cb,
        .on_message_complete = lhp_message_complete_cb
    };

    lua_settop(L, 2);
    lua_getfenv(L, 1);
    assert(lua_gettop(L) == ST_FENV_IDX);

    lua_createtable(L, 10, 0);
    assert(lua_gettop(L) == ST_TODO_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_istable(L, -1));
    assert(lua_gettop(L) == ST_BUFFER_IDX);

    parser->data = L;
    result = http_parser_execute(parser, &settings, str, len);

    assert(lua_gettop(L) == ST_BUFFER_IDX);

    /* Save buffer back to fenv */
    lua_rawseti(L, ST_FENV_IDX, FENV_BUFFER_IDX);

    lua_pushinteger(L, lua_objlen(L, -1));
    lua_pushinteger(L, result);

    return 3;
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
    "    local todo, todo_len, result = execute(...)\n"
    "    for i = 1, todo_len, 2 do\n"
    "        todo[i](todo[i+1])\n"
    "    end\n"
    "    return result\n"
    "end";
static void lhp_push_execute_fn(lua_State* L) {
    int top = lua_gettop(L);
    int ok  = luaL_loadstring(L, lhp_execute_lua);
    assert(0 == ok);

    /* Create environment table: */
    lua_createtable(L, 0, 1);

    lua_pushcfunction(L, lhp_execute);
    lua_setfield(L, -2, "execute");

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
    lua_createtable(L, 0, 2);

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
