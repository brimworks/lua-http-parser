#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include "http-parser/http_parser.h"

#define PARSER_MT "http.parser{parser}"

#define check_parser(L, narg)                                   \
    ((lhttp_parser*)luaL_checkudata((L), (narg), PARSER_MT))

/* The Lua stack indices */
#define ST_FENV_IDX   3
#define ST_BUFFER_IDX 4
#define ST_URL_IDX    5
#define ST_LEN        ST_URL_IDX

/* Callback identifiers are indices into the fenv table where the
 * callback is saved.
 */
#define CB_ON_MESSAGE_BEGIN      1
#define CB_ON_URL                2
#define CB_ON_PATH               3
#define CB_ON_QUERY_STRING       4
#define CB_ON_FRAGMENT           5
#define CB_ON_HEADER_FIELD       6
#define CB_ON_HEADER_VALUE       7
#define CB_ON_HEADERS_COMPLETE   8
#define CB_ON_BODY               9
#define CB_ON_MESSAGE_COMPLETE  10
#define CB_LEN                  sizeof(lhp_callback_names)/sizeof(*lhp_callback_names)

static const char *lhp_callback_names[] = {
    /* The MUST be in the same order as the above callbacks */
    "on_message_begin",
    "on_url",
    "on_path",
    "on_query_string",
    "on_fragment",
    "on_header_field",
    "on_header_value",
    "on_headers_complete",
    "on_body",
    "on_message_complete",
};

/* Non-callback FENV indices. */
#define FENV_BUFFER_IDX         11
#define FENV_URL_IDX            12
#define FENV_LEN                FENV_URL_IDX

#define CB_ID_TO_CB_BIT(cb_id)   (1<<((cb_id)-1))
#define CB_ID_TO_BUF_BIT(cb_id) (1<<((cb_id)+CB_LEN-1))

/* Test/set/remove a bit from the flags field of lhttp_parser.  The
 * FLAG_*_CB() macros test/set/remove the bit that signifies that a
 * callback with that id has been registered in the FENV.  The
 * FLAG_*_BUF() macros test/set/remove the bit that signifies that
 * data is buffered for that callback.
 */
#define FLAG_HAS_CB(flags, cb_id)  ( (flags) &   CB_ID_TO_CB_BIT(cb_id) )
#define FLAG_SET_CB(flags, cb_id)  ( (flags) |=  CB_ID_TO_CB_BIT(cb_id) )
#define FLAG_RM_CB(flags, cb_id)   ( (flags) &= ~CB_ID_TO_CB_BIT(cb_id) )
#define FLAG_HAS_BUF(flags, cb_id) ( (flags) &   CB_ID_TO_BUF_BIT(cb_id) )
#define FLAG_SET_BUF(flags, cb_id) ( (flags) |=  CB_ID_TO_BUF_BIT(cb_id) )
#define FLAG_RM_BUF(flags, cb_id)  ( (flags) &= ~CB_ID_TO_BUF_BIT(cb_id) )

typedef struct lhttp_parser {
    http_parser parser;  /* embedded http_parser. */
    int         flags;   /* See above flag test/set/remove macros. */
    int         cb_id;   /* current callback id. */
    int         buf_len; /* number of buffered chunks for current callback. */
    int         url_len; /* number of buffered chunks for 'on_url' callback. */
} lhttp_parser;

static int lhp_table_concat_and_clear(lua_State *L, int idx, int len) {
    luaL_Buffer   buff;
    int           i;

    assert(NULL != L);

    /* fast path one chunk case. */
    if (1 == len) {
        lua_rawgeti(L, idx, 1);
        /* remove values from buffer. */
        lua_pushnil(L);
        lua_rawseti(L, idx, 1);
        return 0;
    }
    /* do a table concat. */
    luaL_buffinit(L, &buff);
    for(i = 1; i <= len; i++) {
        lua_rawgeti(L, idx, i);
        luaL_addvalue(&buff);
        /* remove values from buffer. */
        lua_pushnil(L);
        lua_rawseti(L, idx, i);
    }
    luaL_pushresult(&buff);
    return 0;
}

static int lhp_flush_event(http_parser* parser, int cb_id, int buf_idx, int *buf_len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State*    L;
    int           len;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* no event to flush. */
    if ( ! cb_id ||
         ! FLAG_HAS_BUF(lparser->flags, cb_id) )
    {
        return 0;
    }

    if ( ! lua_checkstack(L, 5) ) return -1;

    FLAG_RM_BUF(lparser->flags, cb_id);

    /* push event callback function. */
    lua_rawgeti(L, ST_FENV_IDX, cb_id);

    /* get buffer length. */
    len = *buf_len;
    *buf_len = 0; /* reset buffer length. */
    return lhp_table_concat_and_clear(L, buf_idx, len);
}

static int lhp_http_cb(http_parser* parser, int cb_id) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* check if event has a callback function. */
    if ( FLAG_HAS_CB(lparser->flags, cb_id) ) {
        if ( ! lua_checkstack(L, 5) ) return -1;

        /* push event callback function. */
        lua_rawgeti(L, ST_FENV_IDX, cb_id);
        lua_pushnil(L);
    }
    return 0;
}

static int lhp_buffer_data(http_parser* parser, int cb_id, int buf_idx, int *buf_len, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State*    L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* only buffer chunk if event has a callback function. */
    if ( FLAG_HAS_CB(lparser->flags, cb_id) ) {
        if ( ! lua_checkstack(L, 5) ) return -1;

        /* insert event chunk into buffer. */
        FLAG_SET_BUF(lparser->flags, cb_id);
        lua_pushlstring(L, str, len);
        lua_rawseti(L, buf_idx, ++(*buf_len));
    }
    return 0;
}

static int lhp_http_data_cb(http_parser* parser, int cb_id, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    /* flush previous event. */
    if (cb_id != lparser->cb_id) {
        int result = lhp_flush_event(parser, lparser->cb_id, ST_BUFFER_IDX, &(lparser->buf_len));
        if ( 0 != result ) return result;
    }
    lparser->cb_id = cb_id;
    return lhp_buffer_data(parser, cb_id, ST_BUFFER_IDX, &(lparser->buf_len), str, len);
}

static int lhp_flush_url(http_parser* parser) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    assert(NULL != lparser);

    /* flush on_url event. */
    return lhp_flush_event(parser, CB_ON_URL, ST_URL_IDX, &(lparser->url_len));
}

static int lhp_http_no_buffer_cb(http_parser* parser, int cb_id, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* push event callback function. */
    if (FLAG_HAS_CB(lparser->flags, cb_id)) {

        if ( ! lua_checkstack(L, 5) ) return -1;

        lua_rawgeti(L, ST_FENV_IDX, cb_id);
        lua_pushlstring(L, str, len);
    }
    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_BEGIN);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    return lhp_buffer_data(parser, CB_ON_URL, ST_URL_IDX, &(lparser->url_len), str, len);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_PATH, str, len);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_QUERY_STRING, str, len);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_FRAGMENT, str, len);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    /* make sure on_url event was flushed. */
    int result = lhp_flush_url(parser);
    if ( 0 != result ) return result;

    return lhp_http_data_cb(parser, CB_ON_HEADER_FIELD, str, len);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER_VALUE, str, len);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    /* make sure on_url event was flushed. */
    int result = lhp_flush_url(parser);
    if ( 0 != result ) return result;

    /* flush last header event. */
    result = lhp_flush_event(parser, lparser->cb_id, ST_BUFFER_IDX, &(lparser->buf_len));
    if ( 0 != result ) return result;

    return lhp_http_cb(parser, CB_ON_HEADERS_COMPLETE);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_no_buffer_cb(parser, CB_ON_BODY, str, len);
}

static int lhp_message_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_COMPLETE);
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    int cb_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    /* Stack: callbacks */

    lhttp_parser* lparser = (lhttp_parser*)lua_newuserdata(L, sizeof(lhttp_parser));
    http_parser* parser = &(lparser->parser);
    assert(NULL != parser);
    /* Stack: callbacks, userdata */

    lparser->flags   = 0;
    lparser->cb_id   = 0;
    lparser->buf_len = 0;
    lparser->url_len = 0;

    /* Get the metatable: */
    luaL_getmetatable(L, PARSER_MT);
    assert(!lua_isnil(L, -1)/* PARSER_MT found? */);
    /* Stack: callbacks, userdata, metatable */

    /* Copy functions to new fenv table */
    lua_createtable(L, FENV_LEN, 0);
    /* Stack: callbacks, userdata, metatable, fenv */
    for (cb_id = 1; cb_id <= CB_LEN; cb_id++ ) {
        lua_getfield(L, 1, lhp_callback_names[cb_id-1]);
        if ( lua_isfunction(L, -1) ) {
            lua_rawseti(L, -2, cb_id); /* fenv[cb_id] = callback */
            FLAG_SET_CB(lparser->flags, cb_id);
        } else {
            lua_pop(L, 1); /* pop non-function value. */
        }
    }
    /* Create buffer table and add it to the fenv table. */
    lua_createtable(L, 1, 0);
    lua_rawseti(L, -2, FENV_BUFFER_IDX);
    /* if the on_url callback is registered, then create special buffer for url. */
    if (FLAG_HAS_CB(lparser->flags, CB_ON_URL)) {
        /* Create url buffer table and add it to the fenv table. */
        lua_createtable(L, 1, 0);
        lua_rawseti(L, -2, FENV_URL_IDX);
    }
    /* Stack: callbacks, userdata, metatable, fenv */
    lua_setfenv(L, -3);
    /* Stack: callbacks, userdata, metatable */

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
    lhttp_parser* lparser = check_parser(L, 1);
    http_parser*  parser = &(lparser->parser);
    size_t        len;
    size_t        result;
    const char*   str = luaL_checklstring(L, 2, &len);

    static const http_parser_settings settings = {
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

    /* truncate stack to (userdata, string) */
    lua_settop(L, 2);

    lua_getfenv(L, 1);
    assert(lua_istable(L, -1));
    assert(lua_getttop(L) == ST_FENV_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_istable(L, -1));
    assert(lua_getttop(L) == ST_BUFFER_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_URL_IDX);
    assert(lua_istable(L, -1) || !FLAG_HAS_CB(lparser->flags, CB_ON_URL));
    assert(lua_gettop(L) == ST_URL_IDX);

    assert(lua_gettop(L) == ST_LEN);
    lua_pushnil(L);

    /* Stack: (userdata, string, fenv, buffer, url, nil) */
    parser->data = L;

    result = http_parser_execute(parser, &settings, str, len);

    parser->data = NULL;

    /* replace nil place-holder with 'result' code. */
    lua_pushnumber(L, result);
    lua_replace(L, ST_LEN+1);
    /* Transform the stack into a table: */
    len = lua_gettop(L) - ST_URL_IDX;
    return len;
}

static int lhp_should_keep_alive(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    lua_pushboolean(L, http_should_keep_alive(&lparser->parser));
    return 1;
}

static int lhp_is_upgrade(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    lua_pushboolean(L, lparser->parser.upgrade);
    return 1;
}

static int lhp_method(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    switch(lparser->parser.method) {
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
        lua_pushnumber(L, lparser->parser.method);
    }
    return 1;
}

static int lhp_status_code(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    lua_pushnumber(L, lparser->parser.status_code);
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
#ifndef NDEBUG
    int top = lua_gettop(L);
#endif
    int err  = luaL_loadstring(L, lhp_execute_lua);

    if ( err ) lua_error(L);

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
    lua_newtable(L); /* Stack: table */

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
