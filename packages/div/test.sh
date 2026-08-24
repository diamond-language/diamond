#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-drb-package` from the repo root, which sets this up already).
#
# Note: `diamond FILE`/`diamond -e CODE` always prints its program's own
# final top-level expression value plus a trailing newline
# (src/run_source.c's diamond_value_print), independent of anything the
# program itself calls puts() with -- every driver script below ends with
# a puts(...) call, whose own return value (nil) is what actually gets
# echoed that way. Assertions here check that rendered output *contains*
# the expected fragments rather than exact-matching stdout, specifically
# to stay robust to that trailing echo (and to each template's own
# leading literal-text newline) rather than hardcoding either.
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

# --- end-to-end: locals, <%= %> escaping, <%== %> raw, <% %> control
# flow, and quote/backslash/#{-containing literal text all survive the
# round trip through the generated .di file's actual rendered output.
# "hello.html.drb" compiles to a function named hello_html (see
# function_name_for in lib/drb/compiler.di) ---
cat >"$work/hello.html.drb" <<'DRBEOF'
<%# locals: title, items %>
<h1><%= title %></h1>
<p>Raw: <%== "<b>bold</b>" %></p>
<p>Quote test: <%= "she said \"hi\" & <bye>" %></p>
<ul>
<% items.each() do |item| %>
  <li><%= item %></li>
<% end %>
</ul>
DRBEOF

"$diamond" bin/drbc.di "$work/hello.html.drb" "$work/hello.html.di" >/dev/null

cat >"$work/driver.di" <<DRIVEREOF
require "$work/hello.html"
puts(hello_html("My <Title> & \"quote\"", ["apple", "b<a>nana"]))
DRIVEREOF

actual="$("$diamond" "$work/driver.di")"
assert_contains "$actual" '<h1>My &lt;Title&gt; &amp; &quot;quote&quot;</h1>'
assert_contains "$actual" '<p>Raw: <b>bold</b></p>'
assert_contains "$actual" '<p>Quote test: she said &quot;hi&quot; &amp; &lt;bye&gt;</p>'
assert_contains "$actual" $'<li>apple</li>'
assert_contains "$actual" $'<li>b&lt;a&gt;nana</li>'
count=$((count + 1))

# --- a template with no `locals:` directive compiles to a zero-arg
# function ---
cat >"$work/plain.html.drb" <<'DRBEOF'
<p>static</p>
DRBEOF
"$diamond" bin/drbc.di "$work/plain.html.drb" "$work/plain.html.di" >/dev/null
cat >"$work/driver_plain.di" <<DRIVEREOF
require "$work/plain.html"
puts(plain_html())
DRIVEREOF
actual="$("$diamond" "$work/driver_plain.di")"
assert_contains "$actual" '<p>static</p>'
count=$((count + 1))

# --- literal text past the 255-byte string-literal cap (DIAMOND_MAX_
# STRING_LENGTH, src/vm.h) is chunked correctly and reassembles exactly ---
python3 -c "
with open('$work/long.html.drb', 'w') as f:
    f.write('<%# locals: %>\n')
    f.write('x' * 400)
    f.write('\n')
"
"$diamond" bin/drbc.di "$work/long.html.drb" "$work/long.html.di" >/dev/null
cat >"$work/driver_long.di" <<DRIVEREOF
require "$work/long.html"
out = long_html()
puts("len=#{out.length()} stripped=#{out.strip().length()}")
DRIVEREOF
actual="$("$diamond" "$work/driver_long.di")"
assert_contains "$actual" "len=402 stripped=400"
count=$((count + 1))

# --- Drb.escape_html (lib/drb/runtime.di) and a generated template's own
# per-file-named inlined escaping produce the same output for the same
# input -- the two copies are meant to stay in behavioral sync (see
# compiler.di's escape_helper_lines comment) ---
cat >"$work/escape_check.html.drb" <<'DRBEOF'
<%# locals: value %>
<%= value %>
DRBEOF
"$diamond" bin/drbc.di "$work/escape_check.html.drb" "$work/escape_check.html.di" >/dev/null
cat >"$work/driver_escape.di" <<DRIVEREOF
require "$work/escape_check.html"
require "$(pwd)/lib/drb/runtime"
sample = "<a href=\"x\">tom & jerry's</a>"
from_template = escape_check_html(sample).strip()
from_runtime = Drb.escape_html(sample)
puts("match=#{from_template == from_runtime} value=#{from_runtime}")
DRIVEREOF
actual="$("$diamond" "$work/driver_escape.di")"
assert_contains "$actual" "match=true value=&lt;a href=&quot;x&quot;&gt;tom &amp; jerry&#39;s&lt;/a&gt;"
count=$((count + 1))

# --- two different templates required into the SAME program don't
# collide, on either the render function name or the inlined escape
# helper name -- the whole reason function_name_for derives both from
# each input file's own basename rather than a fixed "render"/
# "drb_escape_html" ---
cat >"$work/greeting.html.drb" <<'DRBEOF'
<%# locals: name %><%= name %>!
DRBEOF
"$diamond" bin/drbc.di "$work/greeting.html.drb" "$work/greeting.html.di" >/dev/null
cat >"$work/page.html.drb" <<'DRBEOF'
<%# locals: name %>
<p>Hi, <%== greeting_html(name) %></p>
DRBEOF
"$diamond" bin/drbc.di "$work/page.html.drb" "$work/page.html.di" >/dev/null
cat >"$work/driver_partial.di" <<DRIVEREOF
require "$work/greeting.html"
require "$work/page.html"
puts(page_html("World"))
DRIVEREOF
actual="$("$diamond" "$work/driver_partial.di")"
assert_contains "$actual" '<p>Hi, World!'
count=$((count + 1))

echo "$count drb tests passed"
