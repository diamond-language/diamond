#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"
source_root="$(cd ../.. && pwd)"
root="$(mktemp -d)"
test_project="$(mktemp -d)"
trap 'rm -rf "$root" "$test_project"' EXIT
"$source_root/tools/install_local_cuts.sh" "$test_project" >/dev/null
cd "$test_project"
export REGISTRY_TEST_ROOT="$root"

output="$($diamond -e "require_cut \"registry\"
db = SQLite3.open(\":memory:\")
Registry::Schema.apply(db)
Registry::Schema.apply(db)
store = Registry::BlobStore.new(\"$root\")
bytes = \"archive bytes\"
digest = Digest.sha256(bytes)
size = store.put(digest, bytes)
same = store.read(digest)
rows = db.query(\"SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'releases'\")
\"#{size}|#{same}|#{rows.length()}|#{store.contains?(digest)}\"")"

[[ "$output" == "13|archive bytes|1|true" ]]
output="$("$diamond" -e 'require_cut "registry"
root = ENV["REGISTRY_TEST_ROOT"]
store = Registry::BlobStore.new(root)
digest = Digest.sha256("archive bytes")
rejected = false
begin
  store.put("0000000000000000000000000000000000000000000000000000000000000000", "wrong")
rescue error: ArgumentError
  rejected = error.message() == "blob bytes do not match digest"
end
unless rejected then raise "digest mismatch was not rejected" end
unless Dir.entries(root).length() == 1 then raise "mismatch created a file" end
rejected = false
begin
  store.put(digest, "archive bytes")
rescue error: IOError
  rejected = true
end
unless rejected then raise "duplicate write was not rejected" end
unless store.read(digest) == "archive bytes" then raise "duplicate damaged blob" end
rejected = false
begin
  store.path("../aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
rescue error: ArgumentError
  rejected = true
end
unless rejected then raise "unsafe path was accepted" end
missing = Digest.sha256("missing")
if store.contains?(missing) then raise "missing blob reported present" end
file = File.open(store.path(digest), "w")
file.write("partial")
file.close()
rejected = false
begin
  store.read(digest)
rescue error: IOError
  rejected = error.message() == "stored blob does not match digest"
end
unless rejected then raise "corrupt blob was returned" end
if store.contains?(digest) then raise "corrupt blob reported present" end
"registry rejection and integrity tests passed"
')"
[[ "$output" == "registry rejection and integrity tests passed" ]]
echo "registry package tests passed"

# Two processes race to publish the same binary body. Exactly one wins.
export REGISTRY_TEST_DEST="$root/race"
for worker in 1 2; do
    "$diamond" -e 'bytes = "a" + 0.chr() + "b"
begin
  File.publish(ENV["REGISTRY_TEST_DEST"], bytes)
  "created"
rescue error: IOError
  "exists"
end' >"$test_project/worker-$worker" &
    if [[ $worker == 1 ]]; then first_pid=$!; else second_pid=$!; fi
done
wait "$first_pid"
wait "$second_pid"
[[ "$(sort "$test_project/worker-1" "$test_project/worker-2")" == $'created\nexists' ]]
output="$("$diamond" -e 'file = File.open(ENV["REGISTRY_TEST_DEST"], "r")
bytes = file.read()
file.close()
bytes == "a" + 0.chr() + "b"')"
[[ "$output" == true ]]
# Abandoned staging files do not participate in digest lookup.
printf partial >"$root/.diamond-publish-abandoned"
output="$("$diamond" -e 'require_cut "registry"
store = Registry::BlobStore.new(ENV["REGISTRY_TEST_ROOT"])
bytes = "after interruption"
digest = Digest.sha256(bytes)
store.put(digest, bytes)
store.read(digest) == bytes')"
[[ "$output" == true ]]
echo "atomic publication tests passed"

mkdir "$root/existing-directory"
ln -s "$root/race" "$root/existing-link"
output="$("$diamond" -e 'root = ENV["REGISTRY_TEST_ROOT"]
paths = [root + "/existing-directory", root + "/existing-link", root + "/missing/child"]
count = 0
paths.each() do |path|
  begin
    File.publish(path, "replacement")
    raise "unexpected publish success"
  rescue error: IOError
    count += 1
  end
end
begin
  File.publish(root + "/nul" + 0.chr(), "data")
rescue error: TypeError
  count += 1
end
begin
  File.publish(root + "/nil", nil)
rescue error: TypeError
  count += 1
end
count')"
[[ "$output" == 5 ]]
[[ -L "$root/existing-link" ]]
[[ -d "$root/existing-directory" ]]
[[ "$(find "$root" -name '.diamond-publish-*' | wc -l)" -eq 1 ]]
echo "publication failure cleanup tests passed"
