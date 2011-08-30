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

local function parse_path_query_fragment(uri)
    local path, query, fragment, off
    -- parse path
    path, off = uri:match('([^?]*)()')
    -- parse query
    if uri:sub(off, off) == '?' then
        query, off = uri:match('([^#]*)()', off + 1)
    end
    -- parse fragment
    if uri:sub(off, off) == '#' then
        fragment = uri:sub(off + 1)
        off = #uri
    end
    return path or '/', query, fragment
end

local pipeline = [[
GET / HTTP/1.1
Host: localhost
User-Agent: httperf/0.9.0
Connection: keep-alive

GET /header.jpg HTTP/1.1
Host: localhost
User-Agent: httperf/0.9.0
Connection: keep-alive

]]
pipeline = pipeline:gsub('\n', '\r\n')

function pipeline_test()
    local cbs = {}
    local complete_count = 0
    local body = ''
    function cbs.on_body(chunk)
        if chunk then body = body .. chunk end
    end
    function cbs.on_message_complete()
        complete_count = complete_count + 1
    end

    local parser = lhp.request(cbs)
    ok(parser:execute(pipeline) == #pipeline)
    ok(parser:execute('') == 0)

    ok(parser:should_keep_alive() == true)
    ok(parser:method() == "GET")
    ok(complete_count == 2)
    ok(#body == 0)
end


-- NOTE: http-parser fails if the first response is HTTP 1.0:
-- HTTP/1.0 100 Please continue mate.
-- Which I think is a HTTP spec violation, but other HTTP clients, still work.
-- http-parser will fail by seeing only one HTTP response and putting everything elses in
-- the response body for the first 100 response, until socket close.
local please_continue = [[
HTTP/1.1 100 Please continue mate.

HTTP/1.1 200 OK
Date: Wed, 02 Feb 2011 00:50:50 GMT
Content-Length: 10
Connection: close

0123456789]]
please_continue = please_continue:gsub('\n', '\r\n')

function please_continue_test()
    local cbs = {}
    local complete_count = 0
    local body = ''
    function cbs.on_body(chunk)
        if chunk then body = body .. chunk end
    end
    function cbs.on_message_complete()
        complete_count = complete_count + 1
    end

    local parser = lhp.response(cbs)
    parser:execute(please_continue)
    parser:execute('')

    ok(parser:should_keep_alive() == false)
    ok(parser:status_code() == 200)
    ok(complete_count == 2)
    ok(#body == 10)
end

local connection_close = [[
HTTP/1.1 200 OK
Date: Wed, 02 Feb 2011 00:50:50 GMT
Connection: close

0123456789]]
connection_close = connection_close:gsub('\n', '\r\n')

function connection_close_test()
    local cbs = {}
    local complete_count = 0
    local body = ''
    function cbs.on_body(chunk)
        if chunk then body = body .. chunk end
    end
    function cbs.on_message_complete()
        complete_count = complete_count + 1
    end

    local parser = lhp.response(cbs)
    parser:execute(connection_close)
    parser:execute('')

    ok(parser:should_keep_alive() == false)
    ok(parser:status_code() == 200)
    ok(complete_count == 1)
    ok(#body == 10)
end

function nil_body_test()
    local cbs = {}
    local body_count = 0
    local body = {}
    function cbs.on_body(chunk)
        body[#body+1] = chunk
        body_count = body_count + 1
    end

    local parser = lhp.request(cbs)
    parser:execute("GET / HTTP/1.1\r\n")
    parser:execute("Transfer-Encoding: chunked\r\n")
    parser:execute("\r\n")
    parser:execute("23\r\n")
    parser:execute("This is the data in the first chunk\r\n")
    parser:execute("1C\r\n")
    parser:execute("X and this is the second one\r\n")
    ok(body_count == 2)

    is_deeply(body,
              {"This is the data in the first chunk",
               "X and this is the second one"})

    -- This should cause on_body(nil) to be sent
    parser:execute("0\r\n\r\n")

    ok(body_count == 3)
    ok(#body == 2)
end

function max_events_test()
    -- The goal of this test is to generate the most possible events
    local input_tbl = {
        "GET / HTTP/1.1\r\n",
    }
    -- Generate enough events to trigger a "stack overflow"
    local header_cnt = 3000
    for i=1, header_cnt do
        input_tbl[#input_tbl+1] = "a:\r\n"
    end

    local cbs = {}
    local field_cnt = 0
    function cbs.on_header(field, value)
        field_cnt = field_cnt + 1
    end

    local parser = lhp.request(cbs)
    local input = table.concat(input_tbl)

    local result = parser:execute(input)

    input = input:sub(result)

    -- We should have generated a stack overflow event that should be
    -- handled gracefully... note that
    ok(field_cnt < header_cnt and field_cnt > 1000,
       "Expect " .. header_cnt .. " field events, got " .. field_cnt)

    result = parser:execute(input)

    ok(0 == result, "Parser can not continue after stack overflow ["
       .. tostring(result) .. "]")
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

    cb = {}
    cb_cnt = 0
    cb_val = nil
    function cb.on_url(value)
        cb_cnt = cb_cnt + 1
        cb_val = value
    end

    req = lhp.request(cb)

    req:execute("GET /path")
    req:execute("?qs HTTP/1.1\r\n")
    req:execute("Header-Field:")

    ok(cb_cnt == 1, "on_url flushed? " .. tostring(cb_cnt))
    ok(cb_val == "/path?qs", "on_url buffered")
end

function init_parser()
   local reqs         = {}
   local cur          = nil
   local cb           = {}

   function cb.on_message_begin()
       ok(cur == nil)
       cur = { headers = {} }
   end

   function cb.on_url(value)
       ok(cur.url == nil, "expected [url]=nil, but got ["..tostring(cur.url)..
           "] when setting field [" .. tostring(value) .. "]")
       cur.url = value;
       cur.path, cur.query_string, cur.fragment = parse_path_query_fragment(value)
   end

   function cb.on_body(value)
       if ( nil == cur.body ) then
           cur.body = {}
       end
       table.insert(cur.body, value)
   end

   function cb.on_header(field, value)
       ok(cur.headers[field] == nil)
       cur.headers[field] = value
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

buffer_tests()
basic_tests()
max_events_test()
nil_body_test()
pipeline_test()
please_continue_test()
connection_close_test()

print("1.." .. counter)
