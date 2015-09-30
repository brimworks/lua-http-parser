#!/usr/bin/env lua

package = 'lua-http-parser'
version = 'scm-2'
source = {
    url = 'git://github.com/witchu/lua-http-parser.git'
}
description = {
    summary  = "A Lua binding to Ryan Dahl's http request/response parser.",
    detailed = '',
    homepage = 'http://github.com/brimworks/lua-http-parser',
    license  = 'MIT', --as with Ryan's
}
dependencies = {
    'lua >= 5.1'
}
build = {
    type = 'builtin',
    modules = {
        ['http.parser'] = {
            sources = {
                "http-parser/http_parser.c",
                "lua-http-parser.c"
            }
        }
    }
}
