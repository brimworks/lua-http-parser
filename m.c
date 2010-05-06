/* Copyright 2010 Phoenix Sol <phoenix@burninglabs.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE. 
 */

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include "http-parser/http_parser.h"

#include <stdio.h>

#define LIBNAME "httpparser"
#define MTNAME "HTTPPARSERMT"


static int live_http_parsers = 0;

typedef struct {
	lua_State *L;
	int pkey;	// env ref to parser userdata
	int udkey;  // env ref to this userdata
	int dakey;	// env ref to data table

	// field array
	int fpos;     // for building header fields
	int flen;
	char **field;

	// value array
	int vpos;     // for building header values
	int vlen;
	char **value; // doubles as body array

	http_parser *parser;
	enum http_parser_type type;
	enum { NONE=0, FIELD, VALUE } last_header_element;
	int in_mesg;    // set on message_begin_callback
	int have_body;  // set on first on_body callback and
					    //       cleared in message_begin cb
} LPARSERDATA;

int lhttpparser__count(lua_State* L) {
    lua_pushinteger(L, live_http_parsers);
    return 1;
}

int lhttpparser_message_begin_cb(http_parser *P){
	LPARSERDATA *data = P->data;
	if( data->in_mesg == 1){
		lua_State *L = data->L;
		int key = data->dakey;
		lua_pushinteger(L, key);
		lua_createtable(L, 0, 11);
		lua_newtable(L);
		lua_setfield(L, -2, "headers");
		lua_settable(L, LUA_ENVIRONINDEX);
	}else{
		// init buffers
		data->field = (char**)calloc((data->flen = 256), sizeof(char*));
		if( data->field == NULL )
			return -1; // XXX TODO how to handle this error?
		data->value = (char**)calloc((data->vlen = 256), sizeof(char*));
		if( data->value == NULL )
			return -1; // XXX TODO how to handle this error?
		data->in_mesg = 1;
	}
	data->have_body = 0;
	data->fpos = 0;
	data->vpos = 0;
	return 0;
}

void lhttpparser_record_header( LPARSERDATA *data ){
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	lua_getfield(L, -1, "headers");
	lua_pushstring(L, data->value[0]);
	lua_setfield(L, -2, data->field[0]);

	data->fpos = 0;
	data->vpos = 0;
	data->last_header_element = NONE;
}

int lhttpparser_header_field_cb(http_parser *P, const char *buf, size_t len){  
	LPARSERDATA *data = P->data;
	if( (int)data->last_header_element == VALUE )
		lhttpparser_record_header( data );
	if( data->flen < (data->fpos + len + 1) ){
		size_t new = (size_t)(data->flen += (len * sizeof(char*) + 1));
		data->field = (char**)realloc(data->field, new);
	}
	char* cpy = (char*)calloc(len, sizeof(char));
	if( cpy == NULL )
		return -1; // XXX TODO how to handle this error?
	strncpy(cpy, buf, len);
	data->field[data->fpos++] = cpy;
	data->last_header_element = FIELD;
	return 0;
}

int lhttpparser_header_value_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	if( data->vlen < (data->vpos + len + 1) ){
		size_t new = (size_t)(data->vlen += (len * sizeof(char*) + 1));
		data->value = (char**)realloc(data->value, new);
	}
	char* cpy = (char*)calloc(len, sizeof(char));
	if( cpy == NULL )
		return -1; // XXX TODO how to handle this error?
	strncpy(cpy, buf, len);
	data->value[data->vpos++] = cpy;
	data->last_header_element = VALUE;
	return 0;
}

int lhttpparser_headers_complete_cb(http_parser *P){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	http_parser *parser = data->parser;
	lhttpparser_record_header( data );
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);

	// HTTP version numbers
	lua_pushinteger(L, parser->http_major);
	lua_setfield(L, -2, "major");
	lua_pushinteger(L, parser->http_minor);
	lua_setfield(L, -2, "minor");

	// method or status
	if( data->type == HTTP_REQUEST ){
		switch(parser->method){
			case HTTP_DELETE:		lua_pushstring(L, "DELETE");		break;
			case HTTP_GET:			lua_pushstring(L, "GET");			break;
			case HTTP_HEAD:		lua_pushstring(L, "HEAD");			break;
			case HTTP_POST:		lua_pushstring(L, "POST");			break;
			case HTTP_PUT:			lua_pushstring(L, "PUT");			break;

			// pathlogical
			case HTTP_CONNECT:	lua_pushstring(L, "CONNECT");		break;
			case HTTP_OPTIONS:	lua_pushstring(L, "OPTIONS");		break;
			case HTTP_TRACE:		lua_pushstring(L, "TRACE");		break;

			// webdav
			case HTTP_COPY:		lua_pushstring(L, "COPY");			break;
			case HTTP_LOCK:		lua_pushstring(L, "LOCK");			break;
			case HTTP_MKCOL:		lua_pushstring(L, "MKCOL");		break;
			case HTTP_MOVE:		lua_pushstring(L, "MOVE");			break;
			case HTTP_PROPFIND:	lua_pushstring(L, "PROPFIND");	break;
			case HTTP_PROPPATCH:	lua_pushstring(L, "PROPPATCH");	break;
			case HTTP_UNLOCK:		lua_pushstring(L, "UNLOCK");		break;
		}
		lua_setfield(L, -2, "method");
	}else{
		lua_pushinteger(L, parser->status_code);
		lua_setfield(L, -2, "status");
	}
	// keepalive
	lua_pushboolean(L, http_should_keep_alive(parser));
	lua_setfield(L, -2, "keepalive");

	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "headers_complete");
	return 0;
}

int lhttpparser_request_path_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	lua_pushlstring(L, buf, len);
	lua_setfield(L, -2, "path");
	lua_pop(L, 1);
	return 0;
}

int lhttpparser_request_url_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	lua_pushlstring(L, buf, len);
	lua_setfield(L, -2, "url");
	lua_pop(L, 1);
	return 0;
}

int lhttpparser_fragment_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	lua_pushlstring(L, buf, len);
	lua_setfield(L, -2, "fragment");
	lua_pop(L, 1);
	return 0;
}

int lhttpparser_query_string_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	lua_pushlstring(L, buf, len);
	lua_setfield(L, -2, "query_string");
	lua_pop(L, 1);
   return 0;
}

int lhttpparser_body_cb(http_parser *P, const char *buf, size_t len){
	LPARSERDATA *data = P->data;
	if( data->vlen < (data->vpos + len + 1) ){
      	size_t new = data->vlen + (len*4) * sizeof(char*);
      	data->value = (char**)realloc(data->value, new);
   }
   char *cpy = (char*)malloc(len * sizeof(char*));
	if( cpy == NULL )
		return -1; // XXX TODO how to handle this error ?
	strncpy(cpy, buf, len);
	data->value[data->vpos++] = cpy;
	data->have_body = 1;
   return 0;
}

int lhttpparser_message_complete_cb(http_parser *P){
	LPARSERDATA *data = P->data;
	lua_State *L = data->L;
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	if( data->have_body == 1 ){
   	lua_pushstring(L, data->value[0]);
		lua_setfield(L, -2, "body");
	}
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "message_complete");
	lua_pop(L, 1);
	return 0;
}

static http_parser_settings lhttpparser_settings = {
   .on_message_begin     = lhttpparser_message_begin_cb,  
   .on_header_field      = lhttpparser_header_field_cb,
   .on_header_value      = lhttpparser_header_value_cb,
   .on_path              = lhttpparser_request_path_cb,
   .on_url               = lhttpparser_request_url_cb,
   .on_fragment          = lhttpparser_fragment_cb,
   .on_query_string      = lhttpparser_query_string_cb,
   .on_body              = lhttpparser_body_cb,
   .on_headers_complete  = lhttpparser_headers_complete_cb,
   .on_message_complete  = lhttpparser_message_complete_cb,
};

static int lhttpparser_parse(lua_State *L){
	LPARSERDATA *data = luaL_checkudata(L, 1, MTNAME);
	size_t len;
	const char *buf = luaL_checklstring(L, 2, &len);
	http_parser *parser = data->parser;
	size_t nparsed = http_parser_execute(parser, lhttpparser_settings,
													 buf, len);
	lua_pushboolean(L, (nparsed == len)); // success
	lua_pushboolean(L, parser->upgrade); // upgrade
	return 2;
}

static int lhttpparser_index(lua_State *L){
	LPARSERDATA *data = luaL_checkudata(L, 1, MTNAME);
	lua_rawgeti(L, LUA_ENVIRONINDEX, data->dakey);
	luaL_checkany(L, 2);

	// access the 'parse' method
	// dunno; might be better to throw it into the metatable...
	// but it would have to be done for every message (see message_begin_cb)
	// not sure which way is faster yet
	if( lua_type(L, 2) == LUA_TSTRING &&
		strcmp(lua_tostring(L, 2), "parse") == 0 ){
			lua_pushcfunction(L, lhttpparser_parse);
			return 1;
	}
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	return 1;
}

static int lhttpparser_gc(lua_State *L){
    live_http_parsers--;

    LPARSERDATA *data = luaL_checkudata(L, 1, MTNAME);
    if( data->field != NULL ){
        free(data->field);
    }
    if( data->value != NULL ){
        free(data->value);
    }
    luaL_unref(L, LUA_ENVIRONINDEX, data->dakey);
    luaL_unref(L, LUA_ENVIRONINDEX, data->pkey);
    luaL_unref(L, LUA_ENVIRONINDEX, data->udkey);
    return 0;
}

static int lhttpparser_new(lua_State *L, int ptype){
	LPARSERDATA *data = lua_newuserdata(L, sizeof(LPARSERDATA));
	if( data == NULL ){
		lua_pushnil(L);
		lua_pushstring(L, "unable to create parser data structure");
		return 2;
	}

	http_parser *parser = lua_newuserdata(L, sizeof(http_parser));
	if( parser == NULL ){
		lua_pushnil(L);
		lua_pushstring(L, "unable to create parser");
		return 2;
	}

	data->L = L;
	data->pkey = luaL_ref(L, LUA_ENVIRONINDEX);
	http_parser_init(parser, ptype);
	parser->data = data;
	data->parser = parser;
	data->type = ptype;

	// create data table
	lua_createtable(L, 0, 12);
	lua_newtable(L);
	lua_setfield(L, -2, "headers");
	data->dakey = luaL_ref(L, LUA_ENVIRONINDEX);
	lua_pushvalue(L, -1);
	data->udkey = luaL_ref(L, LUA_ENVIRONINDEX);

	luaL_getmetatable(L, MTNAME);
	lua_setmetatable(L, -2);
        live_http_parsers++;

	return 1;
}

static int lhttpparser_request(lua_State *L){
	return lhttpparser_new(L, HTTP_REQUEST);
}

static int lhttpparser_response(lua_State *L){
	return lhttpparser_new(L, HTTP_RESPONSE);
}

static luaL_reg funcs[] = {{"request", lhttpparser_request},
                           {"response", lhttpparser_response},
                           {"__count", lhttpparser__count},
                           {NULL, NULL}};

static luaL_reg meths[] = {{"__index", lhttpparser_index},
									{"__gc", lhttpparser_gc},
									{NULL, NULL}};

int luaopen_httpparser(lua_State *L){
	luaL_newmetatable(L, MTNAME);
	luaL_register(L, NULL, meths);
	lua_pop(L, 1);

	luaL_register(L, LIBNAME, funcs);
	return 1;
}



