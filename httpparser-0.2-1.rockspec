#!/usr/bin/env lua

package	= 'httpparser'
version	= '0.2-1'
source	= {
	url	= 'http://' --TODO provide source url
}
description	= {
	summary	= "A Lua binding to Ryan Dahl's http request/response parser.",
	detailed	= '',
	homepage	= 'http://github.com/phoenixsol/lua-http-parser',
	license	= 'MIT', --as with Ryan's
}
dependencies = {
	'lua >= 5.1'
}
build	= {
	type		= 'builtin',
	modules	= {
		httpparser	= {
			sources = { 'm.c', 'http-parser/http_parser.c' },
			incdirs = { 'http-parser' }
		}
	}
}
