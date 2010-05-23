#!/usr/bin/env lua
local lhp = require 'http.parser'

requests = {}

-- NOTE: All requests must be version HTTP/1.1:
requests.ab = {
	"GET / HTTP/1.1\r\n",
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

local function init_parser()
   local reqs         = {}
   local cur          = nil
   local cb           = {}
   local header_field = nil

   function cb.on_message_begin()
      assert(cur == nil)
      cur = { headers = {} }
   end

   local fields = { "path", "query_string", "url", "fragment", "body" }
   for _, field in ipairs(fields) do
      cb["on_" .. field] =
         function(value)
            cur[field] = value;
         end
   end

   function cb.on_header_field(value)
      assert(nil == header_field)
      header_field = value
   end

   function cb.on_header_value(value)
      assert(header_field ~= nil)
      assert(cur.headers[header_field] == nil)
      cur.headers[header_field] = value
      header_field = nil
   end

   function cb.on_message_complete()
      assert(nil ~= cur)
      table.insert(reqs, cur)
      cur = nil
   end

   return lhp.request(cb), reqs
end

local parser, reqs = init_parser()

for name, data in pairs(requests) do
   for _, line in ipairs(data) do
      local success, upgrade = parser:execute(line)
      assert(success)
      assert(not upgrade)
   end

   print(reqs[#reqs].headers['User-Agent'])
end
parser = nil



