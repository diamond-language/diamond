# Direct-dispatch smoke test -- calls rack_app/web_app/api_app straight
# from boot.di, no real socket needed (same convention
# examples/project_board's own smoke_test.di uses). Proves the whole
# composed middleware stack behaves correctly on real request/response
# values, not just that each middleware works in isolation (see each
# package's own test.sh for that level).

require "./boot"

def web_request(method, path, headers = {}, body = "")
  {"method": method, "path": path, "headers": headers, "body": body}
end

# Every response's own Set-Cookie is threaded back in as the next
# request's Cookie header, exactly like a real browser holding one
# cookie jar across requests -- letting session state (visits, notes,
# csrf_token) actually persist across this whole test the way it would
# in a browser. CookieSession's own Set-Cookie value is always an Array
# (packages/http's multi-Set-Cookie support -- see that package's
# README), even with only its one cookie in it, so this only ever needs
# the first entry here.
def cookie_pair(set_cookie)
  value = case set_cookie
    when [*lines] then lines[0]
    else set_cookie
  end
  semi = value.index_of(";")
  if semi == nil then value else value.slice(0, semi) end
end

# `cookie` is reassigned after *every* request below, exactly like a
# real browser replacing its cookie jar entry on every Set-Cookie --
# CookieSession re-encrypts the whole session (a fresh AES-GCM nonce
# every call, see packages/cookies' own Cipher.encrypt comment) on every
# single response, so last response's cookie is the only one that
# reflects the latest session state; reusing an older one would silently
# roll the session back to whatever it looked like at that point.
first = web_app(web_request("GET", "/"), {})
if first[0] != 200 then raise "first visit did not return 200" end
if first[1]["Set-Cookie"] == nil then raise "first visit did not set a session cookie" end
if !first[2].include?("Visits this session: 1") then raise "first visit did not show visit count 1" end
cookie = cookie_pair(first[1]["Set-Cookie"])

marker = "X-CSRF-Token': '"
token_start = first[2].index_of(marker) + marker.length()
after_marker = first[2].slice(token_start, first[2].length() - token_start)
csrf_token = after_marker.slice(0, after_marker.index_of("'"))
if csrf_token.length() != 64 then raise "CSRF token was not embedded in the page" end

second = web_app(web_request("GET", "/", {"cookie": cookie}), {})
if !second[2].include?("Visits this session: 2") then raise "session did not persist across requests" end
cookie = cookie_pair(second[1]["Set-Cookie"])

no_token = web_app(web_request("POST", "/notes", {"cookie": cookie}, JSON.stringify({"text": "hi"})), {})
if no_token[0] != 403 then raise "note without a CSRF token was not rejected" end

wrong_token = web_app(web_request("POST", "/notes", {"cookie": cookie, "x-csrf-token": "wrong"}, JSON.stringify({"text": "hi"})), {})
if wrong_token[0] != 403 then raise "note with a wrong CSRF token was not rejected" end

posted = web_app(web_request("POST", "/notes", {"cookie": cookie, "x-csrf-token": csrf_token}, JSON.stringify({"text": "hello world"})), {})
if posted[0] != 200 then raise "note with the correct CSRF token was rejected" end
posted_notes = JSON.parse(posted[2])["notes"]
if posted_notes.length() != 1 || posted_notes[0] != "hello world" then raise "posted note was not stored" end
cookie = cookie_pair(posted[1]["Set-Cookie"])

after_post = web_app(web_request("GET", "/", {"cookie": cookie}), {})
if !after_post[2].include?("hello world") then raise "posted note did not render on the next page load" end
cookie = cookie_pair(after_post[1]["Set-Cookie"])

xss = web_app(web_request("POST", "/notes", {"cookie": cookie, "x-csrf-token": csrf_token}, JSON.stringify({"text": "<script>bad</script>"})), {})
if xss[0] != 200 then raise "second note was rejected" end
cookie = cookie_pair(xss[1]["Set-Cookie"])
escaped_page = web_app(web_request("GET", "/", {"cookie": cookie}), {})
if escaped_page[2].include?("<script>bad</script>") then raise "note text was not HTML-escaped" end
if !escaped_page[2].include?("&lt;script&gt;bad&lt;/script&gt;") then raise "escaped note did not render" end
cookie = cookie_pair(escaped_page[1]["Set-Cookie"])

# Notes are capped at the 5 most recent -- post enough more to prove it.
i = 0
while i < 5
  step = web_app(web_request("POST", "/notes", {"cookie": cookie, "x-csrf-token": csrf_token}, JSON.stringify({"text": "note-#{i}"})), {})
  cookie = cookie_pair(step[1]["Set-Cookie"])
  i = i + 1
end
capped = web_app(web_request("GET", "/", {"cookie": cookie}), {})
if capped[2].include?("hello world") then raise "note list was not capped at 5 entries" end
if !capped[2].include?("note-4") then raise "the most recent note was dropped instead of the oldest" end

# Security headers land on every response, including a rejected one.
if no_token[1]["X-Frame-Options"] != "SAMEORIGIN" then raise "SecurityHeaders did not run on a 403 response" end

# A fresh session (no cookie) is independent of the one above.
fresh = web_app(web_request("GET", "/"), {})
if !fresh[2].include?("Visits this session: 1") then raise "a fresh request shared session state" end

# RateLimit: the shared limit is 30/60s, keyed by session id on the web
# side -- exhaust the *fresh* session's own budget (already spent 1 on
# the request above) without touching the `cookie` session used above.
fresh_cookie = cookie_pair(fresh[1]["Set-Cookie"])
i = 0
limited_status = nil
while i < 30
  limited_status = web_app(web_request("GET", "/", {"cookie": fresh_cookie}), {})[0]
  i = i + 1
end
if limited_status != 429 then raise "RateLimit did not trip after exceeding its limit" end

# Cors: a disallowed origin gets no grant (but the request still
# succeeds -- CORS is a browser-side grant, not a server-side reject);
# an allowed origin gets one; a preflight never reaches api_status.
disallowed = api_app(web_request("GET", "/api/status", {"origin": "https://evil.example"}), {})
if disallowed[0] != 200 then raise "a disallowed-origin request was rejected server-side" end
if disallowed[1]["Access-Control-Allow-Origin"] != nil then raise "a disallowed origin was granted CORS headers" end

allowed = api_app(web_request("GET", "/api/status", {"origin": "https://trusted-partner.example"}), {})
if allowed[1]["Access-Control-Allow-Origin"] != "https://trusted-partner.example" then raise "an allowed origin was not granted CORS headers" end
if JSON.parse(allowed[2])["ok"] != true then raise "the API endpoint did not return its normal body" end

preflight = api_app(web_request("OPTIONS", "/api/status", {"origin": "https://trusted-partner.example", "access-control-request-method": "GET"}), {})
if preflight[0] != 204 || preflight[2] != "" then raise "a CORS preflight reached the app instead of being answered directly" end

# The two path groups are actually independent through rack_app's own
# dispatch: an API request never goes through CookieSession/Csrf.
api_via_dispatch = rack_app(web_request("GET", "/api/status", {"origin": "https://trusted-partner.example"}), {})
if api_via_dispatch[1]["Set-Cookie"] != nil then raise "the API path unexpectedly went through CookieSession" end

unknown = web_app(web_request("GET", "/nope"), {})
if unknown[0] != 404 then raise "an unknown path did not 404" end

puts("smoke_test.passed")
