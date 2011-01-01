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
#define ST_IS_BUFFERED_IDX 4
#define ST_TODO_IDX        5
#define ST_BUFFER_IDX      6
#define ST_NEW_REQUEST_IDX 7

/* The FENV contains string keys for each callback, it also contains
 * the following numeric keys:
 */
#define FENV_BUFFER_IDX      1
#define FENV_IS_BUFFERED_IDX 2
#define FENV_NEW_REQUEST_IDX 3

const static char* default_buffer_tbl = "http.parser{default.buffer}";

#define DEFAULT_BUFFER_TBL (void*)default_buffer_tbl

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

/* Returns the buffer type of the function at idx.  Assumes
 * ST_IS_BUFFERED_IDX is valid.
 */
static int lhp_get_buffered(lua_State*L, int fn_idx) {
    int       buffered;

    lua_pushvalue(L, fn_idx);
    assert(lua_isfunction(L, -1));

    lua_rawget(L, ST_IS_BUFFERED_IDX);

    buffered = lua_tointeger(L, -1);

    lua_pop(L, 1);

    return buffered;
}

/* Move everything out of the buffer table and into the todo table
 * EXCEPT if except_fn_idx is non-zero and except_fn_idx is marked as
 * buffered in which case any buffer type 2's (and the execpt_fn_idx)
 * will remain in the todo table.  So, if except_fn_idx is non-zero
 * then this is really a "partial flush".
 */
static void flush_buffer(lua_State* L, int except_fn_idx) {
    int pop = 0;

    if ( except_fn_idx                      &&
         lua_isfunction(L, except_fn_idx)   &&
         0 != lhp_get_buffered(L, except_fn_idx) )
    {
        lua_pushvalue(L, except_fn_idx);
        pop++;
    }

    lua_createtable(L, 0, 3);
    lua_pushnil(L);

    while ( lua_next(L, ST_BUFFER_IDX) != 0 ) {
        /* <except-fn>?, <new-buffer-tbl>, <key>, <value> */
        if ( ( except_fn_idx && lhp_get_buffered(L, -2) > 1 ) ||
             ( pop &&  lua_rawequal(L, -2, -4) ) )
        {
            /* Put it in the new buffer table */
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -5);
        } else {
            lua_pushvalue(L, -2);
            lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);

            lua_pushvalue(L, -1);
            if ( lua_istable(L, -1) ) table_concat(L);
            lua_rawseti(L, ST_TODO_IDX, lua_objlen(L, ST_TODO_IDX) + 1);
        }
        lua_pop(L, 1);
    }

    lua_replace(L, ST_BUFFER_IDX);

    if ( pop ) lua_pop(L, pop);
}

/* Returns true if the start line was just completed.  We determine
 * that the start line was just completed by checking if this is a
 * "new request" that has http major/minor version set.
 *
 * NOTE: This method is *not* idempotent, simply by calling this
 * method it will no longer return true.
 */
static int is_start_line_completed(lua_State* L, http_parser* parser) {
    if ( lua_toboolean(L, ST_NEW_REQUEST_IDX) &&
         ( parser->http_major || parser->http_minor ) )
    {
        lua_pushboolean(L, 0);
        lua_replace(L, ST_NEW_REQUEST_IDX);
        return 1;
    }
    return 0;
}

/* Post Condition: If a callback exists in FENV for 'name', then the
 * BUFFER table has <func> key set to <false>.
 */
static int lhp_http_cb(http_parser* parser, const char* name) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    lua_getfield(L, ST_FENV_IDX, name);
    if ( ! lua_isfunction(L, -1) ) {
        lua_pop(L, 1);
    } else {
        lua_pushboolean(L, 0);
        lua_rawset(L, ST_BUFFER_IDX);
    }

    /* Any non-string callbacks trigger global flush */
    flush_buffer(L, 0);

    return 0;
}

/* Post Condition: If a callback exists in FENV for 'name', then the
 * BUFFER table has <func> key's table appended with str (and if a
 * table doesn't exist, a new one is created).
 */
static int lhp_http_data_cb(http_parser* parser, const char* name, const char* str, size_t len) {
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    lua_getfield(L, ST_FENV_IDX, name);
    if ( lua_isfunction(L, -1) ) {
        lua_pushvalue(L, -1);
        lua_rawget(L, ST_BUFFER_IDX);
        if ( ! lua_istable(L, -1) ) {
            lua_pop(L, 1);
            lua_createtable(L, 1, 0);
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, ST_BUFFER_IDX);
        }
        lua_pushlstring(L, str, len);
        lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
        lua_pop(L, 1);
    }

    if ( is_start_line_completed(L, parser) ) {
        flush_buffer(L, 0);
    } else {
        flush_buffer(L, -1);
    }
    lua_pop(L, 1);

    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    int        result = lhp_http_cb(parser, "on_message_begin");
    lua_State* L      = (lua_State*)parser->data;

    /* Reset http version and mark this as a "new request" so we know
     * when the status/request line got read and therefore we can push
     * the start_line_complete token.
     */
    parser->http_major = parser->http_minor = 0;
    lua_pushboolean(L, 0);
    lua_replace(L, ST_NEW_REQUEST_IDX);

    return result;
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_url", str, len);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_path", str, len);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, "on_query_string", str, len);
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
    int result;
    result = lhp_http_data_cb(parser, "on_body", str, len);
    return result;
}

static int lhp_message_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, "on_message_complete");
}

/* Returns 0 if function should not have events buffered, 1 if events
 * can be buffered, or 2 if the buffering can only occur after a
 * "flush" index.
 */
static int lhp_get_default_buffered(lua_State* L, int str_idx) {
    int result = 1;
    str_idx = abs_index(L, str_idx);

    lua_pushlightuserdata(L, DEFAULT_BUFFER_TBL);
    lua_rawget(L, LUA_REGISTRYINDEX);
    assert(lua_istable(L, -1));

    lua_pushvalue(L, str_idx);
    lua_rawget(L, -2);

    if ( lua_isnumber(L, -1) ) {
        result = lua_tonumber(L, -1);
    }
    lua_pop(L, 2);

    return result;
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    luaL_checktype(L, 1, LUA_TTABLE);

    http_parser* parser = (http_parser*)lua_newuserdata(L, sizeof(http_parser));
    assert(NULL != parser);

    /* Copy functions to new fenv table */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushnil(L);
    while ( lua_next(L, 1) != 0 ) {
        /* ..., <tbl fenv>, <tbl buffered?>, <key>, <value> */
        if ( lua_isstring(L, -2) && lua_isfunction(L, -1) ) {
            /* Set tbl buffered? */
            lua_pushvalue(L, -1);
            lua_pushinteger(L, lhp_get_default_buffered(L, -3));
            lua_rawset(L, -5);

            /* Set tbl fenv */
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -6);
        }
        lua_pop(L, 1);
    }

    /* Save the is buffered table into the fenv */
    lua_rawseti(L, -2, FENV_IS_BUFFERED_IDX);

    /* Save the buffer table into the fenv */
    lua_createtable(L, 0, 3);
    lua_rawseti(L, -2, FENV_BUFFER_IDX);

    /* Save the "new request" flag into the fenv */
    lua_pushboolean(L, 0);
    lua_rawseti(L, -2, FENV_NEW_REQUEST_IDX);

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
 * <http_parser>, <str>, <fenv>, <buffer-tbl>, <is-buffered-tbl>,
 * <new-request-bool>, <todo-tbl>
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

    /* read-only */
    lua_rawgeti(L, ST_FENV_IDX, FENV_IS_BUFFERED_IDX);
    assert(lua_istable(L, -1));
    assert(lua_gettop(L) == ST_IS_BUFFERED_IDX);

    lua_createtable(L, 10, 0);
    assert(lua_gettop(L) == ST_TODO_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_istable(L, -1));
    assert(lua_gettop(L) == ST_BUFFER_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_NEW_REQUEST_IDX);
    assert(lua_isboolean(L, -1));
    assert(lua_gettop(L) == ST_NEW_REQUEST_IDX);

    parser->data = L;
    result = http_parser_execute(parser, &settings, str, len);

    assert(lua_gettop(L) == ST_NEW_REQUEST_IDX);

    /* Save values back to fenv */
    lua_rawseti(L, ST_FENV_IDX, FENV_NEW_REQUEST_IDX);
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

    /* Default buffer mode */
    lua_pushlightuserdata(L, DEFAULT_BUFFER_TBL);
    lua_createtable(L, 0, 4);

    lua_pushliteral(L, "on_body");
    lua_pushinteger(L, 0);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_url");
    lua_pushinteger(L, 2);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_path");
    lua_pushinteger(L, 2);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_query_string");
    lua_pushinteger(L, 2);
    lua_rawset(L, -3);

    lua_pushliteral(L, "on_fragment");
    lua_pushinteger(L, 2);
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
