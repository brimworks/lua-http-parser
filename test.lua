#!/usr/bin/env lua
local lhp = require 'http.parser'

local expects = {}
local requests = {}

-- NOTE: All requests must be version HTTP/1.1 since we re-use the same HTTP parser for all requests.
requests.ab = {
   "GET /", "foo/t", ".html?", "qst", "ring", "#fr", "ag ", "HTTP/1.1\r\n",
   "Ho",
   "st: loca",
   "lhos",
   "t:8000\r\nUser-Agent: ApacheBench/2.3\r\n",
   "Con", "tent-L", "ength", ": 5\r\n",
   "Accept: */*\r\n\r",
   "\nbody\n",
}

expects.ab = {
   path = "/foo/t.html",
   query_string = "qstring",
   fragment = "frag",
   headers = {
      Host = "localhost:8000",
      ["User-Agent"] = "ApacheBench/2.3",
      Accept = "*/*",
   },
   body = "body\n"
}

requests.httperf = {
	"GET / HTTP/1.1\r\n",
	"Host: localhost\r\n",
	"User-Agent: httperf/0.9.0\r\n\r\n"
}

expects.httperf = {
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

expects.firefox = {
   headers = {
      ["User-Agent"] = "Mozilla/5.0 (X11; U;Linux i686; en-US; rv:1.9.0.15)Gecko/2009102815 Ubuntu/9.04 (jaunty)Firefox/3.0.15",
   }
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

   -- TODO: If you set a url handler and (path or query_string or
   -- fragment) handler, then the url event will not be properly
   -- buffered.
   local fields = { "path", "query_string", "fragment", "body" }
   for _, field in ipairs(fields) do
      cb["on_" .. field] =
         function(value)
            assert(cur[field] == nil, "["..tostring(field).."]=["..tostring(cur[field]).."] .. [" .. tostring(value) .. "]")
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

local function assert_deeply(got, expect, context)
   assert(type(expect) == "table", "Expected [" .. context .. "] to be a table")
   for k, v in pairs(expect) do
      local ctx = context .. "." .. k
      if type(expect[k]) == "table" then
         assert_deeply(got[k], expect[k], ctx)
      else
         assert(got[k] == expect[k], "Expected [" .. ctx .. "] to be '" .. tostring(expect[k]) .. "', but got '" .. tostring(got[k]) .. "'")
      end
   end
end

for name, data in pairs(requests) do
   for _, line in ipairs(data) do
      local bytes_read = parser:execute(line)
      assert(bytes_read == #line, "only ["..tostring(bytes_read).."] bytes read, expected ["..tostring(#line).."]")
   end

   local got    = reqs[#reqs]
   local expect = expects[name]
   assert(parser:method() == "GET", "Method is GET")
   assert_deeply(got, expect, name)
end
parser = nil

print "ok"

