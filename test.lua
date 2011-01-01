#!/usr/bin/env lua

-- Make it easier to test
local src_dir, build_dir = ...
if ( src_dir ) then
    package.path  = src_dir .. "?.lua;" .. package.path
    package.cpath = build_dir .. "?.so;" .. package.cpath
end

local lhp = require 'http.parser'

local counter = 1

function ok(assert_true, desc)
    local msg = ( assert_true and "ok " or "not ok " ) .. counter
    if ( desc ) then
        msg = msg .. " - " .. desc
    end
    print(msg)
    counter = counter + 1
end

function basic_tests()
    local expects  = {}
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
        url = "/foo/t.html?qstring#frag",
        headers = {
            Host = "localhost:8000",
            ["User-Agent"] = "ApacheBench/2.3",
            Accept = "*/*",
        },
        body = { "body\n" }
    }

    requests.no_buff_body = {
        "GET / HTTP/1.1\r\n",
        "Host: foo:80\r\n",
        "Content-Length: 12\r\n",
        "\r\n",
        "chunk1", "chunk2",
    }

    expects.no_buff_body = {
        body = {
            "chunk1", "chunk2"
        }
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

    local parser, reqs = init_parser()

    for name, data in pairs(requests) do
        for _, line in ipairs(data) do
            local bytes_read = parser:execute(line)
            ok(bytes_read == #line, "only ["..tostring(bytes_read).."] bytes read, expected ["..tostring(#line).."] in ".. name)
        end

        local got    = reqs[#reqs]
        local expect = expects[name]
        ok(parser:method() == "GET", "Method is GET")
        is_deeply(got, expect, "Check " .. name)
    end
end

function buffer_tests()
    local cb = {}
    local cb_cnt = 0
    local cb_val

    function cb.on_path(value)
        cb_cnt = cb_cnt + 1
        cb_val = value
    end

    local req = lhp.request(cb)

    req:execute("GET /pa")
    req:execute("th?qs")
    ok(cb_cnt == 1, "on_path flushed?")
    ok(cb_val == "/path", "on_path buffered")
end

function init_parser()
   local reqs         = {}
   local cur          = nil
   local cb           = {}
   local header_field = nil

   function cb.on_message_begin()
       ok(cur == nil)
       cur = { headers = {} }
   end

   local fields = { "path", "query_string", "fragment", "url" }
   for _, field in ipairs(fields) do
       cb["on_" .. field] =
           function(value)
               ok(cur[field] == nil, "["..tostring(field).."]=["..tostring(cur[field]).."] .. [" .. tostring(value) .. "]")
               cur[field] = value;
           end
   end

   function cb.on_body(value)
       if ( nil == cur.body ) then
           cur.body = {}
       end
       table.insert(cur.body, value)
   end

   function cb.on_header_field(value)
       ok(nil == header_field)
       header_field = value
   end

   function cb.on_header_value(value)
       ok(header_field ~= nil)
       ok(cur.headers[header_field] == nil)
       cur.headers[header_field] = value
       header_field = nil
   end

   function cb.on_message_complete()
       ok(nil ~= cur)
       table.insert(reqs, cur)
       cur = nil
   end

   return lhp.request(cb), reqs
end

function is_deeply(got, expect, msg, context)
    if ( type(expect) ~= "table" ) then
        print("# Expected [" .. context .. "] to be a table")
        ok(false, msg)
        return false
    end
    for k, v in pairs(expect) do
        local ctx
        if ( nil == context ) then
            ctx = k
        else
            ctx = context .. "." .. k
        end
        if type(expect[k]) == "table" then
            if ( not is_deeply(got[k], expect[k], msg, ctx) ) then
                return false
            end
        else
            if ( got[k] ~= expect[k] ) then
                print("# Expected [" .. ctx .. "] to be '"
                      .. tostring(expect[k]) .. "', but got '"
                      .. tostring(got[k])
                      .. "'")
                ok(false, msg)
                return false
            end
        end
    end
    if ( nil == context ) then
        ok(true, msg);
    end
    return true
end

basic_tests()
buffer_tests()

print("1.." .. counter)