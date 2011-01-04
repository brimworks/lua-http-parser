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
 * callback is saved.  If you add/remove/change anything about these,
 * be sure to update lhp_callback_names and FLAG_GET_BUF_CB_ID.
 */
#define CB_ON_MESSAGE_BEGIN      1
#define CB_ON_URL                2
#define CB_ON_PATH               3
#define CB_ON_QUERY_STRING       4
#define CB_ON_FRAGMENT           5
#define CB_ON_HEADER             6
#define CB_ON_HEADERS_COMPLETE   7
#define CB_ON_BODY               8
#define CB_ON_MESSAGE_COMPLETE   9
#define CB_LEN                   (sizeof(lhp_callback_names)/sizeof(*lhp_callback_names))

static const char *lhp_callback_names[] = {
    /* The MUST be in the same order as the above callbacks */
    "on_message_begin",
    "on_url",
    "on_path",
    "on_query_string",
    "on_fragment",
    "on_header",
    "on_headers_complete",
    "on_body",
    "on_message_complete",
};

/* Non-callback FENV indices. */
#define FENV_BUFFER_IDX         CB_LEN + 1
#define FENV_URL_IDX            CB_LEN + 2
#define FENV_LEN                FENV_URL_IDX

#define CB_ID_TO_CB_BIT(cb_id)  (1<<(cb_id))
#define CB_ID_TO_BUF_BIT(cb_id) (1<<((cb_id)+CB_LEN))

/* Git the cb_id that has information stored in buff */
#define FLAG_GET_BUF_CB_ID(flags) lhp_buf_bit_to_cb_id \
    ( (flags) &                                    \
      (CB_ID_TO_BUF_BIT(CB_ON_PATH)         |      \
       CB_ID_TO_BUF_BIT(CB_ON_QUERY_STRING) |      \
       CB_ID_TO_BUF_BIT(CB_ON_FRAGMENT)     |      \
       CB_ID_TO_BUF_BIT(CB_ON_HEADER)       |      \
       0) )

/* Test/set/remove a bit from the flags field of lhttp_parser.  The
 * FLAG_*_CB() macros test/set/remove the bit that signifies that a
 * callback with that id has been registered in the FENV.
 *
 * The FLAG_*_BUF() macros test/set/remove the bit that signifies that
 * data is buffered for that callback.
 *
 * The FLAG_*_HFIELD() macros test/set/remove the bit that signifies
 * that the first element of the buffer is the header field key.
 */
#define FLAG_HAS_CB(flags, cb_id)  ( (flags) &   CB_ID_TO_CB_BIT(cb_id) )
#define FLAG_SET_CB(flags, cb_id)  ( (flags) |=  CB_ID_TO_CB_BIT(cb_id) )
#define FLAG_RM_CB(flags, cb_id)   ( (flags) &= ~CB_ID_TO_CB_BIT(cb_id) )

#define FLAG_HAS_BUF(flags, cb_id) ( (flags) &   CB_ID_TO_BUF_BIT(cb_id) )
#define FLAG_SET_BUF(flags, cb_id) ( (flags) |=  CB_ID_TO_BUF_BIT(cb_id) )
#define FLAG_RM_BUF(flags, cb_id)  ( (flags) &= ~CB_ID_TO_BUF_BIT(cb_id) )

#define FLAG_HAS_HFIELD(flags)     ( (flags) & 1 )
#define FLAG_SET_HFIELD(flags)     ( (flags) |= 1 )
#define FLAG_RM_HFIELD(flags)      ( (flags) &= ~1 )

typedef struct lhttp_parser {
    http_parser parser;     /* embedded http_parser. */
    int         flags;      /* See above flag test/set/remove macros. */
    int         buf_len;    /* number of buffered chunks for current callback. */
    int         url_len;    /* number of buffered chunks for 'on_url' callback. */
} lhttp_parser;

/* Translate a "buf bit" into the cb_id.
 */
static inline int lhp_buf_bit_to_cb_id(int flags) {
    switch ( flags ) {
    case CB_ID_TO_BUF_BIT(CB_ON_PATH):         return CB_ON_PATH;
    case CB_ID_TO_BUF_BIT(CB_ON_QUERY_STRING): return CB_ON_QUERY_STRING;
    case CB_ID_TO_BUF_BIT(CB_ON_FRAGMENT):     return CB_ON_FRAGMENT;
    case CB_ID_TO_BUF_BIT(CB_ON_HEADER):       return CB_ON_HEADER;
    }
    return 0;
}

/* Concatinate and remove elements from the table at idx starting at
 * element begin and going to length len.  The final concatinated string
 * is on the top of the stack.
 */
static int lhp_table_concat_and_clear(lua_State *L, int idx, int begin, int len) {
    luaL_Buffer   buff;
    int           real_len = len-begin+1;

    /* Empty table? */
    if ( !real_len ) {
        lua_pushliteral(L, "");
        return 0;
    }

    /* One element? */
    if ( 1 == real_len ) {
        lua_rawgeti(L, idx, begin);
        /* remove values from buffer. */
        lua_pushnil(L);
        lua_rawseti(L, idx, begin);
        return 0;
    }
    /* do a table concat. */
    luaL_buffinit(L, &buff);
    for(; begin <= len; begin++) {
        lua_rawgeti(L, idx, begin);
        luaL_addvalue(&buff);
        /* remove values from buffer. */
        lua_pushnil(L);
        lua_rawseti(L, idx, begin);
    }
    luaL_pushresult(&buff);
    return 0;
}

/* "Flush" the buffer at buf_idx of length buf_len for the callback
 * identified by cb_id.  The CB_ON_HEADER cb_id is flushed by
 * inspecting FLAG_HAS_HFIELD().  If that bit is not set, then the
 * buffer at buf_idx is concatinated into a single string element in
 * the buffer and nothing is pushed on the Lua stack.  Otherwise the
 * buffer table is cleared after pushing the following onto the Lua
 * stack:
 *
 *   CB_ON_HEADER function,
 *   first element of the buffer,
 *   second - length element of the buffer concatinated
 *
 * If cb_id is not CB_ON_HEADER then the buffer table is cleared after
 * pushing the following onto the Lua stack:
 *
 *   cb_id function,
 *   first - length elements of the buffer concatinated
 *   
 */
static int lhp_flush(lhttp_parser* lparser, int cb_id, int buf_idx, int* buf_len) {
    lua_State*    L = (lua_State*)lparser->parser.data;
    int           begin, len, result, top, save;

    assert(cb_id);
    assert(FLAG_HAS_BUF(lparser->flags, cb_id));
    assert(CB_ON_URL==cb_id ?
           ST_URL_IDX    == buf_idx :
           ST_BUFFER_IDX == buf_idx);
    assert(CB_ON_URL==cb_id ?
           &lparser->url_len == buf_len :
           &lparser->buf_len == buf_len);

    if ( ! lua_checkstack(L, 7) ) return -1;

    len   = *buf_len;
    begin = 1;
    top   = lua_gettop(L);

    FLAG_RM_BUF(lparser->flags, cb_id);
    if ( CB_ON_HEADER == cb_id ) {
        if ( FLAG_HAS_HFIELD(lparser->flags) ) {
            /* Push <cnt>, <func>, <arg1>[, <arg2>] */
            lua_pushinteger(L, 3);
            lua_rawgeti(L, ST_FENV_IDX, cb_id);
            lua_rawgeti(L, buf_idx, 1);
            lua_pushnil(L);
            lua_rawseti(L, buf_idx, 1);

            begin    = 2;
            save     = 0;
            *buf_len = 0;
            FLAG_RM_HFIELD(lparser->flags);
        } else {
            /* Save */
            begin    = 1;
            save     = 1;
            *buf_len = 1;
        }
    } else {
        /* Push <cnt>, <func>[, <arg1> */
        lua_pushinteger(L, 2);
        lua_rawgeti(L, ST_FENV_IDX, cb_id);

        begin    = 1;
        save     = 0;
        *buf_len = 0;
    }

    result = lhp_table_concat_and_clear(L, buf_idx, begin, len);
    if ( 0 != result ) {
        lua_settop(L, top);
        return result;
    }

    if ( save ) lua_rawseti(L, buf_idx, 1);

    return 0;
}

/* Puts the str of length len into the buffer table at buf_idx and
 * updates buf_len.  It also sets the buf flag for cb_id.
 */
static int lhp_buffer(lhttp_parser* lparser, int cb_id, int buf_idx, int *buf_len, const char* str, size_t len, int hfield) {
    lua_State* L = (lua_State*)lparser->parser.data;

    assert(cb_id);
    assert(FLAG_HAS_CB(lparser->flags, cb_id));
    assert(CB_ON_URL==cb_id ?
           ST_URL_IDX    == buf_idx :
           ST_BUFFER_IDX == buf_idx);
    assert(CB_ON_URL==cb_id ?
           &lparser->url_len == buf_len :
           &lparser->buf_len == buf_len);

    /* insert event chunk into buffer. */
    FLAG_SET_BUF(lparser->flags, cb_id);
    if ( hfield ) {
        FLAG_SET_HFIELD(lparser->flags);
    }

    lua_pushlstring(L, str, len);
    lua_rawseti(L, buf_idx, ++(*buf_len));

    return 0;
}

/* Push the zero argument event for cb_id.  Post condition:
 *  Lua stack contains 1, <func>
 */
static int lhp_push_nil_event(lhttp_parser* lparser, int cb_id) {
    lua_State* L = (lua_State*)lparser->parser.data;

    assert(FLAG_HAS_CB(lparser->flags, cb_id));

    if ( ! lua_checkstack(L, 5) ) return -1;

    lua_pushinteger(L, 1);
    lua_rawgeti(L, ST_FENV_IDX, cb_id);

    return 0;
}

/* Flush the buffer and/or url as long as it is not the except_cb_id
 * being buffered.
 */
static int lhp_flush_except(lhttp_parser* lparser, int except_cb_id, int flush_url, int hfield) {
    int flush = 0;
    int cb_id = FLAG_GET_BUF_CB_ID(lparser->flags);

    /* flush previous event and/or url */
    if ( cb_id ) {
        if ( cb_id == CB_ON_HEADER ) {
            flush = hfield ^ FLAG_HAS_HFIELD(lparser->flags);
        } else if ( cb_id != except_cb_id ) {
            flush = 1;
        }
    }

    if ( flush ) {
        int result = lhp_flush(lparser, cb_id, ST_BUFFER_IDX, &lparser->buf_len);
        if ( 0 != result ) return result;
    }

    if ( flush_url && FLAG_HAS_BUF(lparser->flags, CB_ON_URL) ) {
        int result = lhp_flush(lparser, CB_ON_URL, ST_URL_IDX, &lparser->url_len);
        if ( 0 != result ) return result;
    }
    return 0;
}

/* The event for cb_id where cb_id takes a string argument.  If
 * flush_url is true, then any buffered information in the URL_IDX
 * will be flushed.
 */
static int lhp_http_data_cb(http_parser* parser, int cb_id, const char* str, size_t len, int flush_url, int hfield) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    int  buf_idx;
    int* buf_len;

    if ( cb_id != CB_ON_URL ) {
        int result = lhp_flush_except(lparser, cb_id, flush_url, hfield);
        if ( 0 != result ) return result;
    }

    if ( ! FLAG_HAS_CB(lparser->flags, cb_id) ) return 0;

    if ( CB_ON_URL == cb_id ) {
        buf_idx = ST_URL_IDX;
        buf_len = &lparser->url_len;
    } else {
        buf_idx = ST_BUFFER_IDX;
        buf_len = &lparser->buf_len;
    }

    return lhp_buffer(lparser, cb_id, buf_idx, buf_len, str, len, hfield);
}

static int lhp_http_cb(http_parser* parser, int cb_id, int flush_url) {
    lhttp_parser* lparser = (lhttp_parser*)parser;

    int result = lhp_flush_except(lparser, cb_id, flush_url, 0);
    if ( 0 != result ) return result;

    if ( ! FLAG_HAS_CB(lparser->flags, cb_id) ) return 0;

    return lhp_push_nil_event(lparser, cb_id);
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_BEGIN, 0);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_URL, str, len, 0, 0);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_PATH, str, len, 0, 0);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_QUERY_STRING, str, len, 0, 0);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_FRAGMENT, str, len, 0, 0);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER, str, len, 1, 0);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER, str, len, 0, 1);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_HEADERS_COMPLETE, 1);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    /* on_headers_complete did any flushing, so just push the cb */
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State*    L = (lua_State*)lparser->parser.data;

    if ( ! FLAG_HAS_CB(lparser->flags, CB_ON_BODY) ) return 0;

    if ( ! lua_checkstack(L, 5) ) return -1;

    lua_pushinteger(L, 2);
    lua_rawgeti(L, ST_FENV_IDX, CB_ON_BODY);
    lua_pushlstring(L, str, len);

    return 0;
}

static int lhp_message_complete_cb(http_parser* parser) {
    /* Send on_body(nil) message to comply with LTN12 */
    int result = lhp_push_nil_event((lhttp_parser*)parser, CB_ON_BODY);
    if ( 0 != result ) return result;

    return lhp_http_cb(parser, CB_ON_MESSAGE_COMPLETE, 0);
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
    assert(lua_gettop(L) == ST_FENV_IDX);

    lua_rawgeti(L, ST_FENV_IDX, FENV_BUFFER_IDX);
    assert(lua_istable(L, -1));
    assert(lua_gettop(L) == ST_BUFFER_IDX);

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
    len = lua_gettop(L) - ST_LEN;

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
    "local c_execute = ...\n"
    "local function execute(result, cnt, cb, arg1, arg2, ...)\n"
    "    if ( not cnt ) then\n"
    "        return result\n"
    "    end\n"
    "    if ( cnt == 3 ) then\n"
    "        cb(arg1, arg2)\n"
    "        return execute(result, ...)\n"
    "    end\n"
    "    if ( cnt == 2 ) then\n"
    "        cb(arg1)\n"
    "        return execute(result, arg2, ...)"
    "    end\n"
    /*   if ( cnt == 1 ) then */
    "    cb()\n"
    "    return execute(result, arg1, arg2, ...)\n"
    "end\n"
    "return function(...)\n"
    "    return execute(c_execute(...))\n"
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
