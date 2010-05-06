#!/usr/bin/env lua
require 'httpparser'

requests = {}

requests.ab = {
	"GET / HTTP/1.0\r\n",
	"Host: localhost:8000\r\n",
	"User-Agent: ApacheBench/2.3\r\n",
	"Accept: */*\r\n\r\n"
}

requests.httperf = {
	"GET / HTTP/1.1\r\n",
	"Host: localhost\r\n",
	"User-Agent: httperf/0.9.0\r\n\r\n"
}

requests.firefox = {
	"GET / HTTP/1.1\r\n",
	"Host: two.local:8000\r\n",
	"User-Agent: Mozilla/5.0 (X11; U;",
	"Linux i686; en-US; rv:1.9.0.15)",
	"Gecko/2009102815 Ubuntu/9.04 (jaunty)",
	"Firefox/3.0.15\r\n",
	"Accept: text/html,application/xhtml+xml,application/xml;",
	"q=0.9,*/*;q=0.8\r\n",
	"Accept-Language:en-gb,en;q=0.5\r\n",
	"Accept-Encoding: gzip,deflate\r\n",
	"Accept-Charset:ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n",
	"Keep-Alive: 300\r\n",
	"Connection:keep-alive\r\n\r\n"
}

print("count=" .. httpparser.__count())

for name, data in pairs(requests) do
	local request = httpparser.request()

	for _, line in ipairs(data) do
		local success, upgrade = request:parse( line )
		assert( success )
		assert( not upgrade )
	end

	assert( request.headers_complete )
	assert( request.message_complete )
	assert( request.major )
	assert( request.minor )
	assert( request.headers )
	assert( request.method )
	assert( request.path )
	assert( request.url )
	-- incomplete; see 'usage.lua'

	local headers = request.headers
	--for k,v in pairs(headers) do print(k,v) end
	print( 'tested ' .. headers['User-Agent'] )

end

collectgarbage("collect")
collectgarbage("collect")

print("count=" .. httpparser.__count())


