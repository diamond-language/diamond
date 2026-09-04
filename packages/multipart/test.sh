#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-multipart-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/multipart\"
$script"
}

# --- a real multipart body: two plain fields and one file field, in
# --- one request, all three parsed correctly ---
actual="$(run_case '
boundary = "----Boundary123"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"title\"\r\n\r\n" +
  "My Cool Theme\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"tags\"\r\n\r\n" +
  "dark, minimal\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"theme_file\"; filename=\"theme.zip\"\r\n" +
  "Content-Type: application/zip\r\n\r\n" +
  "PK-fake-zip-bytes" + "\r\n" +
  "--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
file = result["files"]["theme_file"]
"#{result["fields"]["title"]}|#{result["fields"]["tags"]}|#{file["filename"]}|#{file["content_type"]}|#{file["data"]}"
')"
[[ "$actual" == "My Cool Theme|dark, minimal|theme.zip|application/zip|PK-fake-zip-bytes" ]]
count=$((count + 1))

# --- binary file content (including a NUL byte) round-trips exactly --
# --- Diamond strings are raw byte buffers, and split()/slice() operate
# --- on bytes, not text, so this isn't a special case in the parser,
# --- just worth confirming directly ---
actual="$(run_case '
boundary = "----Boundary123"
binary = "PK" + 1.chr() + "binarydata" + 255.chr() + 254.chr() + 0.chr() + "moretail"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"f\"; filename=\"x.bin\"\r\n" +
  "Content-Type: application/octet-stream\r\n\r\n" +
  binary + "\r\n" +
  "--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
"#{result["files"]["f"]["data"].length()}|#{result["files"]["f"]["data"] == binary}"
')"
[[ "$actual" == "24|true" ]]
count=$((count + 1))

# --- a filename with spaces and special characters (but no embedded
# --- quote -- the one documented limitation) round-trips ---
actual="$(run_case '
boundary = "----Boundary123"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"f\"; filename=\"my theme (v2) [final].zip\"\r\n\r\n" +
  "data\r\n--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
multipart_parse(request)["files"]["f"]["filename"]
')"
[[ "$actual" == "my theme (v2) [final].zip" ]]
count=$((count + 1))

# --- a file part with no Content-Type header defaults to
# --- application/octet-stream ---
actual="$(run_case '
boundary = "----Boundary123"
body = "--#{boundary}\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x\"\r\n\r\ndata\r\n--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
multipart_parse(request)["files"]["f"]["content_type"]
')"
[[ "$actual" == "application/octet-stream" ]]
count=$((count + 1))

# --- fields-only body (no file field at all) parses to an empty
# --- "files" Hash, not nil or an error ---
actual="$(run_case '
boundary = "----Boundary123"
body = "--#{boundary}\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
"#{result["fields"].length()}|#{result["files"].length()}"
')"
[[ "$actual" == "1|0" ]]
count=$((count + 1))

# --- multipart_boundary stops at a trailing "; charset=..." parameter
# --- rather than swallowing it into the boundary value ---
actual="$(run_case 'multipart_boundary("multipart/form-data; boundary=----X; charset=UTF-8")')"
[[ "$actual" == "----X" ]]
count=$((count + 1))

# --- not multipart at all (wrong or missing Content-Type) -> nil, not
# --- an error, so a caller can fall back to Dials::Params itself ---
actual="$(run_case 'multipart_parse({"headers": {"content-type": "application/x-www-form-urlencoded"}, "body": "a=1"})')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

actual="$(run_case 'multipart_parse({"headers": {}, "body": ""})')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- a malformed part -- no Content-Disposition name at all -- fails
# --- the whole parse (nil), not a partial result ---
actual="$(run_case '
boundary = "----Boundary123"
body = "--#{boundary}\r\nContent-Disposition: form-data\r\n\r\nvalue\r\n--#{boundary}--\r\n"
multipart_parse({"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body})
')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- the boundary string never actually appearing in the body (wrong
# --- boundary, or a truncated/corrupted request) -> nil ---
actual="$(run_case '
multipart_parse({"headers": {"content-type": "multipart/form-data; boundary=----X"}, "body": "nothing matches this at all"})
')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- multipart_file_extension: lowercased, no leading dot, multi-dot
# --- names use the last extension, no extension at all -> nil ---
actual="$(run_case '"#{multipart_file_extension("theme.ZIP")}|#{multipart_file_extension("a.b.tar.gz")}|#{multipart_file_extension("noext")}"')"
[[ "$actual" == "zip|gz|nil" ]]
count=$((count + 1))

# --- multipart_save_file: writes the file under `directory` with a
# --- fresh random name (not the submitted filename), returns that
# --- stored name, and the written bytes round-trip exactly ---
upload_dir="$(mktemp -d)"
actual="$(run_case "
file = {\"filename\": \"theme.zip\", \"content_type\": \"application/zip\", \"data\": \"pretend-zip-bytes\"}
stored = multipart_save_file(file, [\"zip\"], \"$upload_dir\")
handle = File.open(\"$upload_dir/#{stored}\", \"r\")
contents = handle.read()
handle.close()
\"#{stored != \"theme.zip\"}|#{stored.end_with?(\".zip\")}|#{contents}\"
")"
[[ "$actual" == "true|true|pretend-zip-bytes" ]]
count=$((count + 1))

# --- multipart_save_file: nil file, and an extension outside
# --- allowed_extensions, both reject without writing anything ---
before_count="$(ls -1 "$upload_dir" | wc -l)"
actual="$(run_case "
rejected_nil = multipart_save_file(nil, [\"zip\"], \"$upload_dir\")
rejected_ext = multipart_save_file({\"filename\": \"virus.exe\", \"content_type\": \"application/octet-stream\", \"data\": \"x\"}, [\"zip\"], \"$upload_dir\")
\"#{rejected_nil}|#{rejected_ext}\"
")"
[[ "$actual" == "nil|nil" ]]
count=$((count + 1))
after_count="$(ls -1 "$upload_dir" | wc -l)"
[[ "$before_count" == "$after_count" ]]
count=$((count + 1))

# --- a name ending in "[]" (several checkboxes sharing one name, the
# --- standard HTML-forms array convention) collects every value into
# --- an Array under the "[]"-stripped key, in submission order; an
# --- ordinary field name is completely unaffected (still a bare
# --- String, still last-value-wins on repetition) ---
actual="$(run_case '
boundary = "----Boundary456"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"platforms[]\"\r\n\r\n" +
  "windows\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"platforms[]\"\r\n\r\n" +
  "macos\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"title\"\r\n\r\n" +
  "first\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"title\"\r\n\r\n" +
  "second\r\n" +
  "--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
platforms = result["fields"]["platforms"]
"#{platforms.length()}|#{platforms[0]}|#{platforms[1]}|#{result["fields"]["title"]}"
')"
[[ "$actual" == "2|windows|macos|second" ]]
count=$((count + 1))

# --- a single "x[]" part still becomes a one-element Array, not a
# --- bare String -- the "[]" suffix is what opts a field into array
# --- handling, not how many parts happened to show up ---
actual="$(run_case '
boundary = "----Boundary789"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"platforms[]\"\r\n\r\n" +
  "linux\r\n" +
  "--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
platforms = result["fields"]["platforms"]
"#{platforms.length()}|#{platforms[0]}"
')"
[[ "$actual" == "1|linux" ]]
count=$((count + 1))

# --- a file field name ending in "[]" (a real multi-file <input>)
# --- collects every part into an Array of file Hashes under the
# --- "[]"-stripped key, in submission order; an ordinary file field
# --- name is completely unaffected (still a bare Hash, still
# --- last-part-wins on repetition) ---
actual="$(run_case '
boundary = "----BoundaryShots"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"screenshots[]\"; filename=\"one.png\"\r\n" +
  "Content-Type: image/png\r\n\r\n" +
  "first-bytes\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"screenshots[]\"; filename=\"two.png\"\r\n" +
  "Content-Type: image/png\r\n\r\n" +
  "second-bytes\r\n" +
  "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"theme_file\"; filename=\"theme.zip\"\r\n\r\n" +
  "zip-bytes\r\n" +
  "--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
shots = result["files"]["screenshots"]
"#{shots.length()}|#{shots[0]["filename"]}|#{shots[0]["data"]}|#{shots[1]["filename"]}|#{shots[1]["data"]}|#{result["files"]["theme_file"]["filename"]}"
')"
[[ "$actual" == "2|one.png|first-bytes|two.png|second-bytes|theme.zip" ]]
count=$((count + 1))

# --- a single "x[]" file part still becomes a one-element Array, not a
# --- bare file Hash -- same "[]" opts in, regardless of count" rule the
# --- field-array case already has ---
actual="$(run_case '
boundary = "----BoundaryOneShot"
body = "--#{boundary}\r\n" +
  "Content-Disposition: form-data; name=\"screenshots[]\"; filename=\"solo.png\"\r\n\r\n" +
  "solo-bytes\r\n--#{boundary}--\r\n"
request = {"headers": {"content-type": "multipart/form-data; boundary=#{boundary}"}, "body": body}
result = multipart_parse(request)
shots = result["files"]["screenshots"]
"#{shots.length()}|#{shots[0]["filename"]}"
')"
[[ "$actual" == "1|solo.png" ]]
count=$((count + 1))

echo "$count multipart tests passed"
