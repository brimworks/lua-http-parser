#!/usr/bin/env lua

package	= 'lua-http-parser'
version	= '0.1-1'
source	= {
	url	= 'http://' --TODO provide source url
}
description	= {
	summary	= "A Lua binding to Ryan Dahl's http request/response parser.",
	detailed	= '',
	homepage	= 'http://github.com/brimworks/lua-http-parser',
	license	= 'MIT', --as with Ryan's
}
dependencies = {
	'lua >= 5.1'
}
build	= {
	type		= 'builtin',
	modules	= {
		["http.parser"]	= {
			sources = { 'lua-http-parser.c', 'http-parser/http_parser.c' },
			incdirs = { 'http-parser' }
		}
	}
}
