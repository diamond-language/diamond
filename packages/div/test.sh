#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-div-package` from the repo root, which sets this up already).
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
# "hello.html.div" compiles to a function named hello_html (see
# function_name_for in lib/div/compiler.di) ---
cat >"$work/hello.html.div" <<'DRBEOF'
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

"$diamond" bin/divc.di "$work/hello.html.div" "$work/hello.html.di" >/dev/null

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
cat >"$work/plain.html.div" <<'DRBEOF'
<p>static</p>
DRBEOF
"$diamond" bin/divc.di "$work/plain.html.div" "$work/plain.html.di" >/dev/null
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
with open('$work/long.html.div', 'w') as f:
    f.write('<%# locals: %>\n')
    f.write('x' * 400)
    f.write('\n')
"
"$diamond" bin/divc.di "$work/long.html.div" "$work/long.html.di" >/dev/null
cat >"$work/driver_long.di" <<DRIVEREOF
require "$work/long.html"
out = long_html()
puts("len=#{out.length()} stripped=#{out.strip().length()}")
DRIVEREOF
actual="$("$diamond" "$work/driver_long.di")"
assert_contains "$actual" "len=402 stripped=400"
count=$((count + 1))

# --- Div.escape_html (lib/div/runtime.di) and a generated template's own
# per-file-named inlined escaping produce the same output for the same
# input -- the two copies are meant to stay in behavioral sync (see
# compiler.di's escape_helper_lines comment) ---
cat >"$work/escape_check.html.div" <<'DRBEOF'
<%# locals: value %>
<%= value %>
DRBEOF
"$diamond" bin/divc.di "$work/escape_check.html.div" "$work/escape_check.html.di" >/dev/null
cat >"$work/driver_escape.di" <<DRIVEREOF
require "$work/escape_check.html"
require "$(pwd)/lib/div/runtime"
sample = "<a href=\"x\">tom & jerry's</a>"
from_template = escape_check_html(sample).strip()
from_runtime = Div.escape_html(sample)
puts("match=#{from_template == from_runtime} value=#{from_runtime}")
DRIVEREOF
actual="$("$diamond" "$work/driver_escape.di")"
assert_contains "$actual" "match=true value=&lt;a href=&quot;x&quot;&gt;tom &amp; jerry&#39;s&lt;/a&gt;"
count=$((count + 1))

# --- two different templates required into the SAME program don't
# collide, on either the render function name or the inlined escape
# helper name -- the whole reason function_name_for derives both from
# each input file's own basename rather than a fixed "render"/
# "div_escape_html" ---
cat >"$work/greeting.html.div" <<'DRBEOF'
<%# locals: name %><%= name %>!
DRBEOF
"$diamond" bin/divc.di "$work/greeting.html.div" "$work/greeting.html.di" >/dev/null
cat >"$work/page.html.div" <<'DRBEOF'
<%# locals: name %>
<p>Hi, <%== greeting_html(name) %></p>
DRBEOF
"$diamond" bin/divc.di "$work/page.html.div" "$work/page.html.di" >/dev/null
cat >"$work/driver_partial.di" <<DRIVEREOF
require "$work/greeting.html"
require "$work/page.html"
puts(page_html("World"))
DRIVEREOF
actual="$("$diamond" "$work/driver_partial.di")"
assert_contains "$actual" '<p>Hi, World!'
count=$((count + 1))

# --- layouts: no special mechanism, just an ordinary template whose
# declared local holds the already-rendered (and already-escaped) child
# output, embedded raw (see README's "Layouts") -- proves both the
# wrapping markup and the child's own escaping survive the composition ---
cat >"$work/child.html.div" <<'DRBEOF'
<%# locals: name %><h1>Welcome, <%= name %></h1>
DRBEOF
"$diamond" bin/divc.di "$work/child.html.div" "$work/child.html.di" >/dev/null
cat >"$work/layout.html.div" <<'DRBEOF'
<%# locals: title, content %><html><head><title><%= title %></title></head><body><%== content %></body></html>
DRBEOF
"$diamond" bin/divc.di "$work/layout.html.div" "$work/layout.html.di" >/dev/null
cat >"$work/driver_layout.di" <<DRIVEREOF
require "$work/child.html"
require "$work/layout.html"
puts(layout_html("Home & <Away>", child_html("<script>World</script>")))
DRIVEREOF
actual="$("$diamond" "$work/driver_layout.di")"
assert_contains "$actual" '<title>Home &amp; &lt;Away&gt;</title>'
assert_contains "$actual" '<body><h1>Welcome, &lt;script&gt;World&lt;/script&gt;</h1>'
assert_contains "$actual" '</body></html>'
count=$((count + 1))

# --- omitting the output path writes under a `.cache/` directory next
# to the source, not beside it directly -- creating `.cache/` itself if
# it doesn't already exist (see README's "Compiling a template") ---
mkdir -p "$work/views"
cat >"$work/views/cached.html.div" <<'DRBEOF'
<%# locals: %><p>cached</p>
DRBEOF
"$diamond" bin/divc.di "$work/views/cached.html.div" >/dev/null
[[ -f "$work/views/.cache/cached.html.di" ]]
[[ ! -f "$work/views/cached.html.di" ]]
cat >"$work/driver_cache.di" <<DRIVEREOF
require "$work/views/.cache/cached.html"
puts(cached_html())
DRIVEREOF
actual="$("$diamond" "$work/driver_cache.di")"
assert_contains "$actual" '<p>cached</p>'
count=$((count + 1))

# --- package-owned batch compiler recursively discovers templates, handles
# spaces in paths, and removes stale generated files before rebuilding ---
mkdir -p "$work/batch/views/.cache" "$work/batch/views/nested folder/.cache"
cat >"$work/batch/views/root.html.div" <<'DRBEOF'
<p>root</p>
DRBEOF
cat >"$work/batch/views/nested folder/child.html.div" <<'DRBEOF'
<p>child</p>
DRBEOF
touch "$work/batch/views/.cache/stale.html.di"
touch "$work/batch/views/nested folder/.cache/stale.html.di"
actual="$(DIAMOND_BIN="$diamond" bin/divc_all.sh "$work/batch/views")"
assert_contains "$actual" "compiled 2 templates under"
[[ -f "$work/batch/views/.cache/root.html.di" ]]
[[ -f "$work/batch/views/nested folder/.cache/child.html.di" ]]
[[ ! -f "$work/batch/views/.cache/stale.html.di" ]]
[[ ! -f "$work/batch/views/nested folder/.cache/stale.html.di" ]]
count=$((count + 1))

# --- directory-qualified naming: two files sharing a basename in
# different subdirectories no longer collide once divc_all.sh's own
# root-relative path reaches function_name_for_relative, and a file
# directly under the root is completely unaffected (same name either
# function would give it) ---
mkdir -p "$work/qualified/views/skins" "$work/qualified/views/users"
cat >"$work/qualified/views/skins/show.html.div" <<'DRBEOF'
<%# locals: name %>
<p>skin show: <%= name %></p>
DRBEOF
cat >"$work/qualified/views/users/show.html.div" <<'DRBEOF'
<%# locals: name %>
<p>user show: <%= name %></p>
DRBEOF
cat >"$work/qualified/views/pagination.html.div" <<'DRBEOF'
<%# locals: page %>
<p>page <%= page %></p>
DRBEOF
DIAMOND_BIN="$diamond" bin/divc_all.sh "$work/qualified/views" >/dev/null

cat >"$work/qualified_driver.di" <<DRIVEREOF
require "$work/qualified/views/skins/.cache/show.html"
require "$work/qualified/views/users/.cache/show.html"
require "$work/qualified/views/.cache/pagination.html"
puts(skins_show_html("Aero"))
puts(users_show_html("alice"))
puts(pagination_html(3))
DRIVEREOF
actual="$("$diamond" "$work/qualified_driver.di")"
assert_contains "$actual" "skin show: Aero"
assert_contains "$actual" "user show: alice"
assert_contains "$actual" "page 3"
count=$((count + 1))

# --- a space in a directory name (a real, already-tested divc_all.sh
# fixture -- see "nested folder" above) sanitizes to "_" in the
# generated name instead of producing a syntax error in the output file ---
mkdir -p "$work/spacedir/views/nested folder"
cat >"$work/spacedir/views/nested folder/child.html.div" <<'DRBEOF'
<p>child</p>
DRBEOF
DIAMOND_BIN="$diamond" bin/divc_all.sh "$work/spacedir/views" >/dev/null
cat >"$work/spacedir_driver.di" <<DRIVEREOF
require "$work/spacedir/views/nested folder/.cache/child.html"
puts(nested_folder_child_html())
DRIVEREOF
actual="$("$diamond" "$work/spacedir_driver.di")"
assert_contains "$actual" "<p>child</p>"
count=$((count + 1))

# --- divc.di invoked directly with just input+output (no name_path,
# the two-argument form every existing caller/test above already uses)
# keeps the old basename-only name -- adding name_path support didn't
# change the default ---
mkdir -p "$work/legacy/sub"
cat >"$work/legacy/sub/show.html.div" <<'DRBEOF'
<p>legacy</p>
DRBEOF
"$diamond" bin/divc.di "$work/legacy/sub/show.html.div" "$work/legacy_show.html.di" >/dev/null
actual="$(cat "$work/legacy_show.html.di")"
assert_contains "$actual" "def show_html()"
count=$((count + 1))

# --- Div.hidden_field_tag escapes both name and value ---
cat >"$work/driver_hidden_field.di" <<DRIVEREOF
require "$(pwd)/lib/div/runtime"
puts(Div.hidden_field_tag("csrf_token", "abc123"))
puts(Div.hidden_field_tag("weird\"name", "tom & jerry's"))
DRIVEREOF
actual="$("$diamond" "$work/driver_hidden_field.di")"
assert_contains "$actual" '<input type="hidden" name="csrf_token" value="abc123">'
assert_contains "$actual" '<input type="hidden" name="weird&quot;name" value="tom &amp; jerry&#39;s">'
count=$((count + 1))

echo "$count div tests passed"
