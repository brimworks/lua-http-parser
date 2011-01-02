#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include "http-parser/http_parser.h"

#define PARSER_MT "http.parser{parser}"

#define check_parser(L, narg)                                   \
    ((http_parser*)luaL_checkudata((L), (narg), PARSER_MT))

/* The indexes into the lua stack contain these items */
#define ST_FENV_IDX   3
#define ST_BUFFER_IDX 4

/* These are the "ids" of the callbacks stored in the fenv table and
 * the buffer tables are stored in the buffer table with these
 * indices.
 */
#define CB_ON_MESSAGE_BEGIN      1
#define CB_ON_PATH               2
#define CB_ON_QUERY_STRING       3
#define CB_ON_URL                4
#define CB_ON_FRAGMENT           5
#define CB_ON_HEADER_FIELD       6
#define CB_ON_HEADER_VALUE       7
#define CB_ON_HEADERS_COMPLETE   8
#define CB_ON_BODY               9
#define CB_ON_MESSAGE_COMPLETE  10

/* The FENV contains the above callback indexes along with these two
 * other indexes
 */
#define FENV_BUFFER_IDX         11

static const char *lhp_callback_names[] = {
  "on_message_begin",
  "on_path",
  "on_query_string",
  "on_url",
  "on_fragment",
  "on_header_field",
  "on_header_value",
  "on_headers_complete",
  "on_body",
  "on_message_complete",
};

#define CB_ID_TO_FLAG(cb_id) (1<<((cb_id)-1))
#define FLAG_TO_CB_ID(flag)  (which_bit(flag))

/* Is there a better way to do the opposite of x = 1<<y?  aka, solve
 * that equasion for y.
 */
static int which_bit(int flag) {
    switch(flag) {
    case 0x001: return 1;
    case 0x002: return 2;
    case 0x004: return 3;
    case 0x008: return 4;
    case 0x010: return 5;
    case 0x020: return 6;
    case 0x040: return 7;
    case 0x080: return 8;
    case 0x100: return 9;
    case 0x200: return 10;
    case 0x400: return 11;
    case 0x800: return 12;
    }
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

static int lhp_flush(lua_State* L, int buffered_cbs) {
    int remove_cbs = 0;
    int cur_cb;

    /* Iterate over the buffer table, concat strings and push them on
     * to the lua stack.
     */
    lua_pushnil(L);
    while ( lua_next(L, ST_BUFFER_IDX) != 0 ) {
        int cb_id   = lua_tointeger(L, -2);
        int cb_flag = CB_ID_TO_FLAG(cb_id);

        if ( ! ( cb_flag & buffered_cbs ) ) {
            if ( ! lua_checkstack(L, 5) ) {
                lua_pop(L, 2);
                return -1;
            }

            /* <cb_id>, <buffer tbl> */
            assert(lua_istable(L, -1));

            table_concat(L);
            assert(lua_isstring(L, -1));

            lua_rawgeti(L, ST_FENV_IDX, cb_id);
            assert(lua_isfunction(L, -1));

            /* TODO: Isn't there a better way to swap values on the
             * stack?
             */

            /* <cb_id>, <buffer str>, <func> */
            lua_insert(L, -2);
            lua_pushvalue(L, -3);
            lua_remove(L, -4);
            /* <func>, <buffer str>, <cb_id> */

            remove_cbs |= cb_flag;
        } else {
            lua_pop(L, 1);
        }
    }

    for ( cur_cb=remove_cbs & -remove_cbs; remove_cbs; cur_cb <<= 1 ) {
        if ( ! ( remove_cbs & cur_cb ) ) continue;
        remove_cbs &= ~cur_cb;
        lua_pushnil(L);
        lua_rawseti(L, ST_BUFFER_IDX, FLAG_TO_CB_ID(cur_cb));
    }
    return 0;
}

static int lhp_http_cb(http_parser* parser, int cb_id) {
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;

    if ( lhp_flush(L, 0) ) return -1;

    if ( ! lua_checkstack(L, 5) ) return -1;

    /* push event callback function. */
    lua_rawgeti(L, ST_FENV_IDX, cb_id);
    if ( lua_isfunction(L, -1) ) {
        lua_pushnil(L);
    } else {
        lua_pop(L, 1); /* pop non-function value. */
    }
    return 0;
}

static int lhp_http_data_cb(http_parser* parser, int cb_id, const char* str, size_t len, int buffered_cbs) {
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;

    if ( lhp_flush(L, buffered_cbs) ) return -1;

    lua_rawgeti(L, ST_FENV_IDX, cb_id);
    if ( ! lua_isfunction(L, -1) ) {
        lua_pop(L, 1);
    } else if ( ! (CB_ID_TO_FLAG(cb_id) & buffered_cbs) ) {
        if ( ! lua_checkstack(L, 5) ) {
            lua_pop(L, 1);
            return -1;
        }

        /* Bypass the "buffer" since nothing is getting buffered */
        lua_pushlstring(L, str, len);
    } else {
        lua_rawgeti(L, ST_BUFFER_IDX, cb_id);
        if ( ! lua_istable(L, -1) ) {
            lua_pop(L, 1);
            lua_createtable(L, 1, 0);
            lua_pushvalue(L, -1);
            lua_rawseti(L, ST_BUFFER_IDX, cb_id);
        }
        lua_pushlstring(L, str, len);
        lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
        lua_pop(L, 2);
    }

    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_BEGIN);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_URL, str, len,
                            CB_ID_TO_FLAG(CB_ON_URL)          |
                            CB_ID_TO_FLAG(CB_ON_PATH)         |
                            CB_ID_TO_FLAG(CB_ON_QUERY_STRING) |
                            CB_ID_TO_FLAG(CB_ON_FRAGMENT));
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_PATH, str, len,
                            CB_ID_TO_FLAG(CB_ON_URL) |
                            CB_ID_TO_FLAG(CB_ON_PATH));
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_QUERY_STRING, str, len,
                            CB_ID_TO_FLAG(CB_ON_URL) |
                            CB_ID_TO_FLAG(CB_ON_QUERY_STRING));
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_FRAGMENT, str, len,
                            CB_ID_TO_FLAG(CB_ON_URL)  |
                            CB_ID_TO_FLAG(CB_ON_FRAGMENT));
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER_FIELD, str, len,
                            CB_ID_TO_FLAG(CB_ON_HEADER_FIELD));
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER_VALUE, str, len,
                            CB_ID_TO_FLAG(CB_ON_HEADER_VALUE));
}

static int lhp_headers_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_HEADERS_COMPLETE);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_BODY, str, len, 0);
}

static int lhp_message_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_COMPLETE);
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    int idx;
    luaL_checktype(L, 1, LUA_TTABLE);

    http_parser* parser = (http_parser*)lua_newuserdata(L, sizeof(http_parser));
    assert(NULL != parser);

    luaL_getmetatable(L, PARSER_MT);
    assert(!lua_isnil(L, -1)/* PARSER_MT found? */);

    /* Copy functions to new fenv table */
    lua_createtable(L, FENV_BUFFER_IDX, 0);

    /* <cbs>, <ud>, <mt>, <fenv> */
    for ( idx=0; idx < sizeof(lhp_callback_names) / sizeof(char*); idx++ ) {
        lua_getfield(L, 1, lhp_callback_names[idx]);
        if ( lua_isfunction(L, -1) ) {
            lua_rawseti(L, -2, idx+1);
        } else {
            lua_pop(L, 1);
        }
    }

    lua_createtable(L, FENV_BUFFER_IDX-1, 0);
    lua_rawseti(L, -2, FENV_BUFFER_IDX);

    lua_setfenv(L, -3);

    http_parser_init(parser, type);
    parser->data = NULL;

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
    assert(lua_gettop(L) == ST_FENV_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_gettop(L) == ST_BUFFER_IDX);
    assert(lua_istable(L, -1));

    lua_pushnil(L);
    /* <userdata>, <string>, <fenv tbl>, <buffer tbl>, <nil> */

    parser->data = L;
    result = http_parser_execute(parser, &settings, str, len);

    /* replace nil place-holder with 'result' code. */
    lua_pushnumber(L, result);
    lua_replace(L, ST_BUFFER_IDX+1);

    return lua_gettop(L) - ST_BUFFER_IDX;
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
    "local execute = ...\n"
    "local function execute_lua(result, event_cb, chunk, ...)\n"
    "  if event_cb then\n"
         /* call current event callback. */
    "    event_cb(chunk)\n"
         /* handle next event from stack. */
    "    return execute_lua(result, ...)\n"
    "  end\n"
       /* done no more events on stack. */
    "  return result\n"
    "end\n"
    "return function(...)\n"
    "  return execute_lua(execute(...))\n"
    "end";
static void lhp_push_execute_fn(lua_State* L) {
    int top = lua_gettop(L);
    int ok  = luaL_loadstring(L, lhp_execute_lua);
    assert(0 == ok);

    lua_pushcfunction(L, lhp_execute);
    lua_call(L, 1, 1);

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
