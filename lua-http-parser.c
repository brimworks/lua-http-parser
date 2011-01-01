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
#define ST_BUFFER_IDX      4
#define ST_TODO_IDX        5

/* The FENV contains callbacks for each of the CB_IDX_* values and it
 * contains a buffer table.
 */
#define FENV_BUFFER_IDX      CB_IDX_MAX+1

/* The ID of each callback is given a unique bit so it can be used in
 * a bitset.
 */
#define CB_URL_ID           0x001
#define CB_PATH_ID          0x002
#define CB_QUERY_STRING_ID  0x004
#define CB_FRAGMENT_ID      0x008
#define CB_HEADER_FIELD_ID  0x010
#define CB_HEADER_VALUE_ID  0x020
#define CB_BODY_ID          0x040
#define CB_MESSAGE_BEGIN    0x080
#define CB_HEADERS_COMPLETE 0x100
#define CB_MESSAGE_COMPLETE 0x200

/* The index for the callback is represented with these numbers.
 */
#define CB_IDX_URL              1
#define CB_IDX_PATH             2
#define CB_IDX_QUERY_STRING     3
#define CB_IDX_FRAGMENT         4
#define CB_IDX_HEADER_FIELD     5
#define CB_IDX_HEADER_VALUE     6
#define CB_IDX_BODY             7
#define CB_IDX_MESSAGE_BEGIN    8
#define CB_IDX_HEADERS_COMPLETE 9
#define CB_IDX_MESSAGE_COMPLETE 10
#define CB_IDX_MAX              10

/* Light user data used to represent the registered "name" to callback
 * table.
 */
const char* NAME_TO_CB_IDX = "http.parser{NAME_TO_CB_IDX}";

/* Map a bitset element into the index used to store buffered output.
 */
static int bitset_to_idx(int bitset_elm) {
    switch ( bitset_elm ) {
    case CB_URL_ID:          return 1;
    case CB_PATH_ID:         return 2;
    case CB_QUERY_STRING_ID: return 3;
    case CB_FRAGMENT_ID:     return 4;
    case CB_HEADER_FIELD_ID: return 5;
    case CB_HEADER_VALUE_ID: return 6;
    case CB_BODY_ID:         return 7;
    }
    assert(!"Reachable");
    return 0;
}

/* Map the index of the buffered output back to a bitset element.
 */
static int cb_idx_to_id(int idx) {
    switch ( idx ) {
    case 1: return CB_URL_ID;
    case 2: return CB_PATH_ID;
    case 3: return CB_QUERY_STRING_ID;
    case 4: return CB_FRAGMENT_ID;
    case 5: return CB_HEADER_FIELD_ID;
    case 6: return CB_HEADER_VALUE_ID;
    case 7: return CB_BODY_ID;
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
        int cb_idx = lua_tointeger(L, -2);
        int cb_id  = cb_idx_to_id(cb_idx);

        if ( ! ( cb_id & except_bitset ) ) {
            /* Flush it */
            lua_rawgeti(L, ST_FENV_IDX, cb_idx);
            if ( lua_isfunction(L, -1) ) {
                lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
                assert(lua_istable(L, -1));
                table_concat(L);
                lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
            } else {
                /* If we ever allow people to disable a callback in
                 * the middle of a parse, then this assert will be
                 * invalid.
                 */
                assert(!"Reachable");
                lua_pop(L, 2);
            }
            remove_bitset |= cb_id;
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
 * exists for cb_idx, then the pair <func>, <false> is pushed into the
 * TODO table.
 */
static int lhp_http_cb(http_parser* parser, int cb_idx) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    flush_buffer(L, 0);

    lua_rawgeti(L, ST_FENV_IDX, cb_idx);
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
static int lhp_http_data_cb(http_parser* parser, int cb_idx, const char* str, size_t len, int except_bitset) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    flush_buffer(L, except_bitset);

    lua_rawgeti(L, ST_FENV_IDX, cb_idx);
    if ( ! lua_isfunction(L, -1) ) {
        lua_pop(L, 1);
    } else if ( ! (cb_idx_to_id(cb_idx) & except_bitset) ) {
        /* Bypass the "buffer" since nothing is getting buffered */
        lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
        lua_pushlstring(L, str, len);
        lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
    } else {
        lua_rawgeti(L, ST_BUFFER_IDX, cb_idx);
        if ( ! lua_istable(L, -1) ) {
            lua_pop(L, 1);
            lua_createtable(L, 1, 0);
            lua_pushvalue(L, -1);
            lua_rawseti(L, ST_BUFFER_IDX, cb_idx);
        }
        lua_pushlstring(L, str, len);
        lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
        lua_pop(L, 2);
    }
    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_IDX_MESSAGE_BEGIN);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_URL, str, len, CB_URL_ID | CB_PATH_ID | CB_QUERY_STRING_ID | CB_FRAGMENT_ID);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_PATH, str, len, CB_URL_ID | CB_PATH_ID);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_QUERY_STRING, str, len, CB_URL_ID | CB_QUERY_STRING_ID);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_FRAGMENT, str, len, CB_URL_ID | CB_FRAGMENT_ID);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_HEADER_FIELD, str, len, CB_HEADER_FIELD_ID);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_HEADER_VALUE, str, len, CB_HEADER_VALUE_ID);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_IDX_HEADERS_COMPLETE);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_IDX_BODY, str, len, 0);
}

static int lhp_message_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_IDX_MESSAGE_COMPLETE);
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    luaL_checktype(L, 1, LUA_TTABLE);

    http_parser* parser = (http_parser*)lua_newuserdata(L, sizeof(http_parser));
    assert(NULL != parser);

    /* Push the fenv table */
    lua_newtable(L);

    /* Push the name2idx table */
    lua_pushlightuserdata(L, (void*)NAME_TO_CB_IDX);
    lua_rawget(L, LUA_REGISTRYINDEX);
    assert(lua_istable(L, -1));

    /* Copy functions to fenv table */
    lua_pushnil(L);
    while ( lua_next(L, 1) != 0 ) {
        lua_pushvalue(L, -2);
        lua_rawget(L, -4);
        /* ..., <tbl fenv>, <name2idx>, <key>, <value>, <idx> */
        if ( lua_isnumber(L, -1) && lua_isfunction(L, -2) ) {
            lua_pushvalue(L, -2);
            lua_rawset(L, -6);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 2);
        }
    }

    /* Remove the name2idx table */
    lua_pop(L, 1);

    /* Save the buffer table into the fenv */
    lua_createtable(L, CB_IDX_MAX, 0);
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

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_istable(L, -1));
    assert(lua_gettop(L) == ST_BUFFER_IDX);

    lua_createtable(L, 10, 0);
    assert(lua_gettop(L) == ST_TODO_IDX);

    parser->data = L;
    result = http_parser_execute(parser, &settings, str, len);

    assert(lua_gettop(L) == ST_TODO_IDX);

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

    lua_pushlightuserdata(L, (void*)NAME_TO_CB_IDX);
    lua_createtable(L, 0, CB_IDX_MAX);

    lua_pushliteral(L, "on_url");
    lua_pushinteger(L, CB_IDX_URL);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_path");
    lua_pushinteger(L, CB_IDX_PATH);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_query_string");
    lua_pushinteger(L, CB_IDX_QUERY_STRING);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_fragment");
    lua_pushinteger(L, CB_IDX_FRAGMENT);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_header_field");
    lua_pushinteger(L, CB_IDX_HEADER_FIELD);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_header_value");
    lua_pushinteger(L, CB_IDX_HEADER_VALUE);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_body");
    lua_pushinteger(L, CB_IDX_BODY);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_message_begin");
    lua_pushinteger(L, CB_IDX_MESSAGE_BEGIN);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_headers_complete");
    lua_pushinteger(L, CB_IDX_HEADERS_COMPLETE);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_message_complete");
    lua_pushinteger(L, CB_IDX_MESSAGE_COMPLETE);
    lua_rawset(L, -3);

    lua_rawset(L, LUA_REGISTRYINDEX);

    /* export http.parser */
    lua_createtable(L, 0, 2);

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
