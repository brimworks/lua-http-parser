
-- api illustration ( don't try to run this )

local req = httpparser.request()
--local res = httpparser.response()

while connected do

   repeat
		local success, upgrade = req:parse( sock:read(bufsiz) )

		if not success and not upgrade then
			return handle_error( connection, sock, whatever )
		end

		if upgrade then
			--upgrade protocol
		end

	until req.message_complete
   
   local version			= response.version
   local status			= resoponse.status
   local content_length	= response.content_length
	local headers			= response.headers
   local body				= response.body

	-- ...
end

-- parser object attributes:
request_example = {
 major			= 1,
 minor			= 1,
 method			= 'GET',
 keepalive		= true,
 path				= '/',
 url				= 'http://foo.com/?x=6#bar',
 fragment		= 'bar',
 query_string	= 'x=6',
 headers			=  a_table,
 body				= '...',
 headers_complete = true,
 message_complete = true,
}  -- 12 items

response_example = {
 major		= 1,
 minor		= 1,
 status		= 200,
 keepalive	= true,
 headers		= a_table,
 body			= '...',
 headers_complete = true,
 message_complete = true,
}  -- 7 items

