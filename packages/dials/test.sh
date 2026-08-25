#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-dials-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/dials\"
$script"
}

# --- literal path match ---
actual="$(run_case '
def home(request, context, params) = Dials::Response.text(200, "home")
router = Dials::Router.new()
router.get("/", home)
puts(router.dispatch({"path": "/", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "[200, {Content-Type: text/plain}, home]"
count=$((count + 1))

# --- :id segment capture ---
actual="$(run_case '
def show(request, context, params) = Dials::Response.text(200, "id=#{params["id"]}")
router = Dials::Router.new()
router.get("/authors/:id", show)
puts(router.dispatch({"path": "/authors/42", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "id=42"
count=$((count + 1))

# --- multiple :segments, and a literal suffix segment (/edit) ---
actual="$(run_case '
def edit(request, context, params) = Dials::Response.text(200, "editing #{params["id"]}")
def show(request, context, params) = Dials::Response.text(200, "showing #{params["id"]}")
router = Dials::Router.new()
router.get("/authors/:id/edit", edit)
router.get("/authors/:id", show)
puts(router.dispatch({"path": "/authors/7/edit", "method": "GET", "body": ""}, {}))
puts(router.dispatch({"path": "/authors/7", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "editing 7"
assert_contains "$actual" "showing 7"
count=$((count + 1))

# --- verb mismatch falls through to 404, not a match ---
actual="$(run_case '
def show(request, context, params) = Dials::Response.text(200, "shown")
router = Dials::Router.new()
router.get("/authors/:id", show)
puts(router.dispatch({"path": "/authors/1", "method": "POST", "body": ""}, {}))
')"
assert_contains "$actual" "404"
assert_contains "$actual" "not found"
count=$((count + 1))

# --- no route matches at all -> 404 ---
actual="$(run_case '
router = Dials::Router.new()
puts(router.dispatch({"path": "/nope", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" '[404, {Content-Type: text/plain}, not found: /nope]'
count=$((count + 1))

# --- a path param overrides a same-named query param ---
actual="$(run_case '
def show(request, context, params) = Dials::Response.text(200, "id=#{params["id"]}")
router = Dials::Router.new()
router.get("/authors/:id", show)
puts(router.dispatch({"path": "/authors/5?id=999", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "id=5"
count=$((count + 1))

# --- GET query-string params parsed (no path capture involved) ---
actual="$(run_case '
def search(request, context, params) = Dials::Response.text(200, "q=#{params["q"]}")
router = Dials::Router.new()
router.get("/search", search)
puts(router.dispatch({"path": "/search?q=diamond", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "q=diamond"
count=$((count + 1))

# --- POST body params parsed ---
actual="$(run_case '
def create(request, context, params) = Dials::Response.text(201, "name=#{params["name"]}")
router = Dials::Router.new()
router.post("/authors", create)
puts(router.dispatch({"path": "/authors", "method": "POST", "body": "name=Ada"}, {}))
')"
assert_contains "$actual" "name=Ada"
count=$((count + 1))

# --- POST body params are percent-decoded, not just "+"-decoded -- a
# comma (not in the URL-safe set) submits as %2C; Params.decode used to
# only handle "+" -> " ", leaving %XX sequences untouched ---
actual="$(run_case '
def create(request, context, params) = Dials::Response.text(201, "country=#{params["country"]}")
router = Dials::Router.new()
router.post("/authors", create)
puts(router.dispatch({"path": "/authors", "method": "POST", "body": "country=Bahamas%2C+Some+Region"}, {}))
')"
assert_contains "$actual" "country=Bahamas, Some Region"
count=$((count + 1))

# --- Dials::Response.redirect / .not_found shapes ---
actual="$(run_case '
puts(Dials::Response.redirect("/authors", "moved"))
puts(Dials::Response.not_found("/nope"))
')"
assert_contains "$actual" '[302, {Location: /authors}, moved]'
assert_contains "$actual" '[404, {Content-Type: text/plain}, not found: /nope]'
count=$((count + 1))

# --- route filters receive merged params and nil allows the action ---
actual="$(run_case '
def remember_id(request, context, params)
  context["filtered_id"] = params["id"]
  nil
end
def filtered_show(request, context, params) = Dials::Response.text(200, "filter=#{context["filtered_id"]} action=#{params["id"]}")
router = Dials::Router.new()
router.get("/authors/:id", filtered_show, [remember_id])
puts(router.dispatch({"path": "/authors/42?id=wrong", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" "filter=42 action=42"
count=$((count + 1))

# --- a filter response short-circuits later filters and the action ---
actual="$(run_case '
def deny(request, context, params)
  context["first_filter"] = true
  Dials::Response.redirect("/login", "authentication required")
end
def should_not_filter(request, context, params)
  raise "later filter ran"
end
def should_not_run(request, context, params)
  raise "action ran"
end
router = Dials::Router.new()
router.post("/authors", should_not_run, [deny, should_not_filter])
puts(router.dispatch({"path": "/authors", "method": "POST", "body": "name=Ada"}, {}))
')"
assert_contains "$actual" '[302, {Location: /login}, authentication required]'
count=$((count + 1))

# --- filters are scoped to their own route ---
actual="$(run_case '
def deny(request, context, params) = Dials::Response.text(403, "denied")
def public_action(request, context, params) = Dials::Response.text(200, "public")
def private_action(request, context, params) = Dials::Response.text(200, "private")
router = Dials::Router.new()
router.get("/public", public_action)
router.get("/private", private_action, [deny])
puts(router.dispatch({"path": "/public", "method": "GET", "body": ""}, {}))
puts(router.dispatch({"path": "/private", "method": "GET", "body": ""}, {}))
')"
assert_contains "$actual" '[200, {Content-Type: text/plain}, public]'
assert_contains "$actual" '[403, {Content-Type: text/plain}, denied]'
count=$((count + 1))

echo "$count dials tests passed"
