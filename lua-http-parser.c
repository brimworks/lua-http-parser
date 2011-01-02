#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include "http-parser/http_parser.h"

#define PARSER_MT "http.parser{parser}"

#define check_parser(L, narg)                                   \
    ((lhttp_parser*)luaL_checkudata((L), (narg), PARSER_MT))

#define grow_stack(L, count)            \
    if(!lua_checkstack(L, count)) {     \
        printf("Lua stack-overflow\n"); \
        return -1;                      \
    }

/* The Lua stack indices */
#define FENV_IDX 3
#define BUFFER_IDX 4
#define URL_IDX 5

/* These are indices into the fenv table. */
  /* callback indices. */
#define CB_NONE                  0
#define CB_MIN_CB_ID             1
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
#define CB_MAX_CB_ID            10
  /* non-callback indices. */
#define FENV_BUFFER_IDX         11
#define FENV_URL_IDX            12
#define FENV_MAX_IDX            12

static const char *lhp_callback_names[] = {
  NULL, /* unused 0 idx. */
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

static const int lhp_buffer_callback[] = {
  0, /* unused 0 idx. */
  0, /* on_message_begin */
  1, /* on_path */
  1, /* on_query_string */
  1, /* on_url */
  1, /* on_fragment */
  1, /* on_header_field */
  1, /* on_header_value */
  0, /* on_headers_complete */
  0, /* on_body */
  0, /* on_message_complete */
};

#define CB_ID_TO_HAS_FUNC_FLAG(cb_id) (1<<(cb_id))

#define CB_FLAG_TEST_HAS_FUNC(flags, cb_id) (((flags) & (CB_ID_TO_HAS_FUNC_FLAG(cb_id))) != 0)
#define CB_FLAG_SET_HAS_FUNC(flags, cb_id) (flags) |= (CB_ID_TO_HAS_FUNC_FLAG(cb_id))
#define CB_FLAG_UNSET_HAS_FUNC(flags, cb_id) (flags) &= ~(CB_ID_TO_HAS_FUNC_FLAG(cb_id))

#define CB_ID_TO_HAS_DATA_FLAG(cb_id) (1<<(CB_MAX_CB_ID + (cb_id)))

#define CB_FLAG_TEST_HAS_DATA(flags, cb_id) (((flags) & (CB_ID_TO_HAS_DATA_FLAG(cb_id))) != 0)
#define CB_FLAG_SET_HAS_DATA(flags, cb_id) (flags) |= (CB_ID_TO_HAS_DATA_FLAG(cb_id))
#define CB_FLAG_UNSET_HAS_DATA(flags, cb_id) (flags) &= ~(CB_ID_TO_HAS_DATA_FLAG(cb_id))

typedef struct lhttp_parser {
    http_parser parser;  /* embedded http_parser. */
    int         flags;   /* has_func & has_data flags for each callback. */
    int         cb_id;   /* current callback id. */
    int         buf_len; /* number of buffered chunks for current callback. */
    int         url_len; /* number of buffered chunks for 'on_url' callback. */
} lhttp_parser;

static int lhp_table_concat(lua_State *L, int idx, int len) {
    luaL_Buffer   b;
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
    luaL_buffinit(L, &b);
    for(i = 1; i <= len; i++) {
        lua_rawgeti(L, idx, i);
        luaL_addvalue(&b);
        /* remove values from buffer. */
        lua_pushnil(L);
        lua_rawseti(L, idx, i);
    }
    luaL_pushresult(&b);
    return 0;
}

static int lhp_flush_event(http_parser* parser, int cb_id, int buf_idx, int *buf_len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State*    L;
    int           len;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    if (CB_NONE == cb_id) {
        /* no event to flush. */
        return 0;
    }
    if (!CB_FLAG_TEST_HAS_DATA(lparser->flags, cb_id)) {
        return 0;
    }
    CB_FLAG_UNSET_HAS_DATA(lparser->flags, cb_id);

    grow_stack(L, 2);

    /* push event callback function. */
    lua_rawgeti(L, FENV_IDX, cb_id);
    /* get buffer length. */
    len = *buf_len;
    *buf_len = 0; /* reset buffer length. */
    return lhp_table_concat(L, buf_idx, len);
}

static int lhp_http_cb(http_parser* parser, int cb_id) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* check if event has a callback function. */
    if (CB_FLAG_TEST_HAS_FUNC(lparser->flags, cb_id)) {
        grow_stack(L, 2);
        /* push event callback function. */
        lua_rawgeti(L, FENV_IDX, cb_id);
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
    if (CB_FLAG_TEST_HAS_FUNC(lparser->flags, cb_id)) {
        grow_stack(L, 2);
        /* insert event chunk into buffer. */
        CB_FLAG_SET_HAS_DATA(lparser->flags, cb_id);
        lua_pushlstring(L, str, len);
        lua_rawseti(L, buf_idx, ++(*(buf_len)));
    }
    return 0;
}

static int lhp_http_data_cb(http_parser* parser, int cb_id, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    /* flush previous event. */
    if (cb_id != lparser->cb_id) {
        lhp_flush_event(parser, lparser->cb_id, BUFFER_IDX, &(lparser->buf_len));
    }
    lparser->cb_id = cb_id;
    return lhp_buffer_data(parser, cb_id, BUFFER_IDX, &(lparser->buf_len), str, len);
}

static int lhp_flush_url(http_parser* parser) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    assert(NULL != lparser);

    /* flush on_url event. */
    return lhp_flush_event(parser, CB_ON_URL, URL_IDX, &(lparser->url_len));
}

static int lhp_flush_body(http_parser* parser) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State* L;
    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);

    /* check if body had any data. */
    if (!CB_FLAG_TEST_HAS_DATA(lparser->flags, CB_ON_BODY)) {
        return 0;
    }
    CB_FLAG_UNSET_HAS_DATA(lparser->flags, CB_ON_BODY);

    /* push event callback function. */
    grow_stack(L, 2);
    lua_rawgeti(L, FENV_IDX, CB_ON_BODY);
    lua_pushnil(L);

    return 0;
}

static int lhp_http_no_buffer_cb(http_parser* parser, int cb_id, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    lua_State* L;

    assert(NULL != parser);
    L = (lua_State*)parser->data;
    assert(NULL != L);
    grow_stack(L, 2);

    /* push event callback function. */
    if (CB_FLAG_TEST_HAS_FUNC(lparser->flags, cb_id)) {
        lua_rawgeti(L, FENV_IDX, cb_id);
        CB_FLAG_SET_HAS_DATA(lparser->flags, cb_id);
        lua_pushlstring(L, str, len);
    }
    return 0;
}

static int lhp_message_begin_cb(http_parser* parser) {
    return lhp_http_cb(parser, CB_ON_MESSAGE_BEGIN);
}

static int lhp_path_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_PATH, str, len);
}

static int lhp_query_string_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_QUERY_STRING, str, len);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    return lhp_buffer_data(parser, CB_ON_URL, URL_IDX, &(lparser->url_len), str, len);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_FRAGMENT, str, len);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    /* make sure on_url event was flushed. */
    lhp_flush_url(parser);
    return lhp_http_data_cb(parser, CB_ON_HEADER_FIELD, str, len);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_HEADER_VALUE, str, len);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    lhttp_parser* lparser = (lhttp_parser*)parser;
    /* make sure on_url event was flushed. */
    lhp_flush_url(parser);
    /* flush last header event. */
    lhp_flush_event(parser, lparser->cb_id, BUFFER_IDX, &(lparser->buf_len));
    return lhp_http_cb(parser, CB_ON_HEADERS_COMPLETE);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_no_buffer_cb(parser, CB_ON_BODY, str, len);
}

static int lhp_message_complete_cb(http_parser* parser) {
    lhp_flush_body(parser);
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

    lparser->flags = 0;
    lparser->cb_id = CB_NONE;
    lparser->buf_len = 0;
    lparser->url_len = 0;

    /* Get the metatable: */
    luaL_getmetatable(L, PARSER_MT);
    assert(!lua_isnil(L, -1)/* PARSER_MT found? */);
    /* Stack: callbacks, userdata, metatable */

    /* Copy functions to new fenv table */
    lua_createtable(L, FENV_MAX_IDX, 0);
    /* Stack: callbacks, userdata, metatable, fenv */
    for (cb_id = CB_MIN_CB_ID; cb_id <= CB_MAX_CB_ID; cb_id++) {
        lua_getfield(L, 1, lhp_callback_names[cb_id]);
        if ( lua_isfunction(L, -1) ) {
            lua_rawseti(L, -2, cb_id); /* fenv[cb_id] = callback */
            CB_FLAG_SET_HAS_FUNC(lparser->flags, cb_id);
        } else {
            lua_pop(L, 1); /* pop non-function value. */
        }
    }
    /* Create buffer table and add it to the fenv table. */
    lua_createtable(L, 1, 0);
    lua_rawseti(L, -2, FENV_BUFFER_IDX);
    /* if the on_url callback is registered, then create special buffer for url. */
    if (CB_FLAG_TEST_HAS_FUNC(lparser->flags, CB_ON_URL)) {
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

    /* make sure there is room for 3 tables and at least 1 event. */
    grow_stack(L, 3 + 2);

    /* get fenv from userdata.  Stack: (userdata, string, fenv) */
    lua_getfenv(L, 1);
    /* get buffer from fenv.  Stack: (userdata, string, fenv, buffer) */
    lua_rawgeti(L, FENV_IDX, FENV_BUFFER_IDX);
    /* get url buffer from fenv.  Stack: (userdata, string, fenv, buffer, url) */
    lua_rawgeti(L, FENV_IDX, FENV_URL_IDX);
    /* push place-holder value for 'results'.  Stack: (userdata, string, fenv, buffer, url, nil) */
    lua_pushnil(L);

    parser->data = L;

    result = http_parser_execute(parser, &settings, str, len);

    /* replace nil place-holder with 'result' code. */
    lua_pushnumber(L, result);
    lua_replace(L, URL_IDX+1);
    /* Transform the stack into a table: */
    len = lua_gettop(L) - URL_IDX;
    return len;
}

static int lhp_should_keep_alive(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    http_parser*  parser = &(lparser->parser);
    lua_pushboolean(L, http_should_keep_alive(parser));
    return 1;
}

static int lhp_is_upgrade(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    http_parser*  parser = &(lparser->parser);
    lua_pushboolean(L, parser->upgrade);
    return 1;
}

static int lhp_method(lua_State* L) {
    lhttp_parser* lparser = check_parser(L, 1);
    http_parser*  parser = &(lparser->parser);
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
    lhttp_parser* lparser = check_parser(L, 1);
    http_parser*  parser = &(lparser->parser);
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
    lua_newtable(L); /* Stack: table */

    lua_pushcfunction(L, lhp_request);
    lua_setfield(L, -2, "request");

    lua_pushcfunction(L, lhp_response);
    lua_setfield(L, -2, "response");

    return 1;
}
