#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include "http-parser/http_parser.h"

#define PARSER_MT "http.parser{parser}"

#define check_parser(L, narg)                                   \
    ((http_parser*)luaL_checkudata((L), (narg), PARSER_MT))

/* The index which contains the userdata fenv */
#define FENV_IDX 3

/* These are indices into the fenv table. */
#define CB_FLAGS_IDX             1
#define CB_MIN_CB_ID             2
#define CB_ON_MESSAGE_BEGIN      2
#define CB_ON_PATH               3
#define CB_ON_QUERY_STRING       4
#define CB_ON_URL                5
#define CB_ON_FRAGMENT           6
#define CB_ON_HEADER_FIELD       7
#define CB_ON_HEADER_VALUE       8
#define CB_ON_HEADERS_COMPLETE   9
#define CB_ON_BODY              10
#define CB_ON_MESSAGE_COMPLETE  11
#define CB_MAX_CB_ID            11

static const char *lhp_callback_names[] = {
  NULL, /* unused 0 idx. */
  NULL, /* FLAGS idx. */
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
  0, /* FLAGS idx. */
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

#define CB_ID_TO_FLAG(cb_id) (1<<(cb_id))

#define CB_FLAG_TEST_HAS_DATA(flags, cb_id) (((flags) & (CB_ID_TO_FLAG(cb_id))) != 0)
#define CB_FLAG_SET_HAS_DATA(flags, cb_id) (flags) |= (CB_ID_TO_FLAG(cb_id))
#define CB_FLAG_UNSET_HAS_DATA(flags, cb_id) (flags) &= ~(CB_ID_TO_FLAG(cb_id))

typedef struct lhp_State {
    lua_State* L;
    int        flags;
} lhp_State;

static int lhp_http_end_cb(http_parser* parser, int cb_id) {
    lhp_State* lhp;
    lua_State* L;
    assert(NULL != parser);
    lhp = (lhp_State*)parser->data;
    assert(NULL != lhp);
    L = lhp->L;
    assert(NULL != L);
    assert(lua_checkstack(L, 2));

    if (!CB_FLAG_TEST_HAS_DATA(lhp->flags, cb_id)) {
        return 0;
    }
    CB_FLAG_UNSET_HAS_DATA(lhp->flags, cb_id);
    /* push event callback function. */
    lua_rawgeti(L, FENV_IDX, cb_id);
    lua_pushnil(L);
    return 0;
}

static int lhp_http_cb(http_parser* parser, int cb_id) {
    lhp_State* lhp;
    lua_State* L;
    assert(NULL != parser);
    lhp = (lhp_State*)parser->data;
    assert(NULL != lhp);
    L = lhp->L;
    assert(NULL != L);
    assert(lua_checkstack(L, 2));

    /* push event callback function. */
    lua_rawgeti(L, FENV_IDX, cb_id);
    if ( lua_isfunction(L, -1) ) {
        lua_pushnil(L);
    } else {
        lua_pop(L, 1); /* pop non-function value. */
    }
    return 0;
}

static int lhp_http_data_cb(http_parser* parser, int cb_id, const char* str, size_t len) {
    lhp_State* lhp;
    lua_State* L;
    assert(NULL != parser);
    lhp = (lhp_State*)parser->data;
    assert(NULL != lhp);
    L = lhp->L;
    assert(NULL != L);
    assert(lua_checkstack(L, 2));

    /* push event callback function. */
    lua_rawgeti(L, FENV_IDX, cb_id);
    if ( lua_isfunction(L, -1) ) {
        CB_FLAG_SET_HAS_DATA(lhp->flags, cb_id);
        lua_pushlstring(L, str, len);
    } else {
        lua_pop(L, 1); /* pop non-function value. */
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
    lhp_http_end_cb(parser, CB_ON_PATH);
    return lhp_http_data_cb(parser, CB_ON_QUERY_STRING, str, len);
}

static int lhp_url_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_URL, str, len);
}

static int lhp_fragment_cb(http_parser* parser, const char* str, size_t len) {
    lhp_http_end_cb(parser, CB_ON_QUERY_STRING);
    return lhp_http_data_cb(parser, CB_ON_FRAGMENT, str, len);
}

static int lhp_header_field_cb(http_parser* parser, const char* str, size_t len) {
    lhp_http_end_cb(parser, CB_ON_PATH);
    lhp_http_end_cb(parser, CB_ON_QUERY_STRING);
    lhp_http_end_cb(parser, CB_ON_FRAGMENT);
    lhp_http_end_cb(parser, CB_ON_URL);
    lhp_http_end_cb(parser, CB_ON_HEADER_VALUE);
    return lhp_http_data_cb(parser, CB_ON_HEADER_FIELD, str, len);
}

static int lhp_header_value_cb(http_parser* parser, const char* str, size_t len) {
    lhp_http_end_cb(parser, CB_ON_HEADER_FIELD);
    return lhp_http_data_cb(parser, CB_ON_HEADER_VALUE, str, len);
}

static int lhp_headers_complete_cb(http_parser* parser) {
    lhp_http_end_cb(parser, CB_ON_PATH);
    lhp_http_end_cb(parser, CB_ON_QUERY_STRING);
    lhp_http_end_cb(parser, CB_ON_FRAGMENT);
    lhp_http_end_cb(parser, CB_ON_URL);
    lhp_http_end_cb(parser, CB_ON_HEADER_VALUE);
    return lhp_http_cb(parser, CB_ON_HEADERS_COMPLETE);
}

static int lhp_body_cb(http_parser* parser, const char* str, size_t len) {
    return lhp_http_data_cb(parser, CB_ON_BODY, str, len);
}

static int lhp_message_complete_cb(http_parser* parser) {
    lhp_http_end_cb(parser, CB_ON_BODY);
    return lhp_http_cb(parser, CB_ON_MESSAGE_COMPLETE);
}

static int lhp_init(lua_State* L, enum http_parser_type type) {
    int cb_id;
    luaL_checktype(L, 1, LUA_TTABLE);
    /* Stack: callbacks */

    http_parser* parser = (http_parser*)lua_newuserdata(L, sizeof(http_parser));
    assert(NULL != parser);
    /* Stack: callbacks, userdata */

    /* Get the metatable: */
    luaL_getmetatable(L, PARSER_MT);
    assert(!lua_isnil(L, -1)/* PARSER_MT found? */);
    /* Stack: callbacks, userdata, metatable */

    /* Get function 'make_event_buffer' from metatable. */
    lua_pushvalue(L, lua_upvalueindex(1));

    /* Copy functions to new fenv table */
    lua_createtable(L, CB_MAX_CB_ID+1, 0);
    /* Stack: callbacks, userdata, metatable, make_event_buffer, fenv */
    for (cb_id = CB_MIN_CB_ID; cb_id <= CB_MAX_CB_ID; cb_id++) {
        lua_getfield(L, 1, lhp_callback_names[cb_id]);
        if ( lua_isfunction(L, -1) ) {
            /* check if this callback event should be buffered. */
            if ( lhp_buffer_callback[cb_id] ) {
                lua_pushvalue(L, -3); /* dup make_event_buffer. */
                lua_insert(L, -2);
                lua_call(L, 1, 1);
            }
        } else {
            lua_pop(L, 1); /* pop non-function value. */
            lua_pushboolean(L, 0);
        }
        lua_rawseti(L, -2, cb_id); /* fenv[cb_id] = callback or false */
    }
    /* Stack: callbacks, userdata, metatable, make_event_buffer, fenv */
    lua_setfenv(L, -4);
    /* Stack: callbacks, userdata, metatable, make_event_buffer */
    lua_pop(L, 1);
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
    lhp_State    lhp;
    http_parser* parser = check_parser(L, 1);
    size_t       len;
    size_t       result;
    const char*  str = luaL_checklstring(L, 2, &len);
    luaL_Buffer  buf;
    int          cb_id;

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

    /* truncate stack to (userdata, string) */
    lua_settop(L, 2);
    /* get fenv from userdata.  Stack: (userdata, string, fenv) */
    lua_getfenv(L, 1);
    /* push place-holder value for 'results'.  Stack: (userdata, string, fenv, nil) */
    lua_pushnil(L);

    lhp.L = L;
    lhp.flags = 0;
    parser->data = &lhp;

    /* restore flags */
    lua_rawgeti(L, FENV_IDX, CB_FLAGS_IDX);
    if ( lua_isnumber(L, -1) ) {
        lhp.flags = lua_tointeger(L, -1);
    }
    lua_pop(L, 1); /* pop flags. */

    result = http_parser_execute(parser, &settings, str, len);

    /* store event flags. */
    if ( 0 != lhp.flags && 0 != len) {
        /* Save flags to fenv table. */
        lua_pushnumber(L, lhp.flags);
        lua_rawseti(L, FENV_IDX, CB_FLAGS_IDX);
    } else {
        /* clear old flags from fenv table. */
        lua_pushnil(L);
        lua_rawseti(L, FENV_IDX, CB_FLAGS_IDX);
    }

    /* replace nil place-holder with 'result' code. */
    lua_pushnumber(L, result);
    lua_replace(L, FENV_IDX+1);
    /* Transform the stack into a table: */
    len = lua_gettop(L) - FENV_IDX;
    return len;
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

/* This 'make_event_buffer' function creates a lua closure to buffer input events
 * until a 'nil' is received, then it calls the callback with the buffered data. */
static const char* lhp_make_event_buffer_lua =
    "local event_cb = ...\n"
    "local concat = table.concat\n"
    "local buffer = {}\n"
    "local count = 0\n"
    "return function(chunk)\n"
    "  if chunk == nil then\n"
    "    chunk = concat(buffer)\n"
    "    for i=1,count do buffer[i] = nil end\n"
    "    count = 0\n"
    "    return event_cb(chunk)\n"
    "  end\n"
    "  count = count + 1\n"
    "  buffer[count] = chunk\n"
    "end";
static void lhp_push_make_event_buffer_fn(lua_State* L) {
    int top = lua_gettop(L);
    int ok  = luaL_loadstring(L, lhp_make_event_buffer_lua);
    assert(0 == ok);

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

    /* make the 'make_event_buffer' function an upvalue for the request/response functions. */
    lhp_push_make_event_buffer_fn(L);
    /* Stack: table, make_event_buffer */
    lua_pushvalue(L, -1);
    /* Stack: table, make_event_buffer, make_event_buffer */
    lua_pushcclosure(L, lhp_request, 1);
    /* Stack: table, make_event_buffer, func */
    lua_setfield(L, -3, "request");
    /* Stack: table, make_event_buffer */

    lua_pushcclosure(L, lhp_response, 1);
    /* Stack: table, func */
    lua_setfield(L, -2, "response");
    /* Stack: table */

    return 1;
}
