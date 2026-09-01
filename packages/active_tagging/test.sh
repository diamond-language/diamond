#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-active-tagging-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/active_tagging\"
db = SQLite3.open(\":memory:\")
db.execute(\"CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)\")
db.execute(\"CREATE TABLE taggings (id INTEGER PRIMARY KEY, tag_id INTEGER NOT NULL, taggable_id INTEGER NOT NULL)\")
ActiveTagging::Tag.configure(ActiveRecord::Repository.new(Arel.table(\"tags\"), build_active_tagging_tag, \"id\", nil, build_active_tagging_tag_validator()))
ActiveTagging::Tagging.configure(ActiveRecord::Repository.new(Arel.table(\"taggings\"), build_active_tagging_tagging, \"id\"))
$script"
}

assert_eq() {
    local actual="$1" expected="$2"
    if [[ "$actual" != "$expected" ]]; then
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    fi
    count=$((count + 1))
}

# --- Tag.normalize_name: lowercases, trims, and collapses internal
# --- whitespace runs to a single hyphen ---
actual="$(run_case '"#{ActiveTagging::Tag.normalize_name("  Dark Mode  ")}|#{ActiveTagging::Tag.normalize_name("Retro   Wave")}"')"
assert_eq "$actual" "dark-mode|retro-wave"

# --- Tag.parse_names: splits on comma, normalizes each, dedupes
# --- (first occurrence wins), and drops anything that normalizes to
# --- blank; nil/blank input is [] ---
actual="$(run_case '
names = ActiveTagging::Tag.parse_names("Dark Mode, dark-mode, Minimal, , Dark Mode")
"#{names.length()} #{names[0]} #{names[1]}"
')"
assert_eq "$actual" "2 dark-mode minimal"

actual="$(run_case '"#{ActiveTagging::Tag.parse_names(nil).length()} #{ActiveTagging::Tag.parse_names("   ").length()}"')"
assert_eq "$actual" "0 0"

# --- Tag.find_or_create: reuses an existing row by name rather than
# --- creating a duplicate ---
actual="$(run_case '
first = ActiveTagging::Tag.find_or_create(db, "dark-mode")
second = ActiveTagging::Tag.find_or_create(db, "dark-mode")
"#{first.id() == second.id()} #{ActiveTagging::Tag.all().count(db)}"
')"
assert_eq "$actual" "true 1"

# --- the configured validator rejects an unnormalized name (spaces,
# --- uppercase) and one over 40 characters ---
actual="$(run_case '
begin
  ActiveTagging::Tag.new({"name": "Not Normalized"}).save(db)
  "no error"
rescue error: ActiveRecord::ValidationError
  "#{error.errors()}"
end
')"
assert_eq "$actual" "[name is invalid]"

actual="$(run_case '
begin
  ActiveTagging::Tag.new({"name": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}).save(db)
  "no error"
rescue error: ActiveRecord::ValidationError
  "#{error.errors()}"
end
')"
assert_eq "$actual" "[name is too long (maximum 40)]"

# --- Tagging.set_tags: attaches the given names (creating Tags as
# --- needed), and Tagging.tags_for reads them back in some order ---
actual="$(run_case '
ActiveTagging::Tagging.set_tags(db, 100, ["dark-mode", "minimal"])
tags = ActiveTagging::Tagging.tags_for(db, 100)
names = []
tags.each() do |t| names.push(t.name()) end
"#{names.length()} #{names.include?("dark-mode")} #{names.include?("minimal")}"
')"
assert_eq "$actual" "2 true true"

# --- Tagging.set_tags on a second call replaces the set -- a dropped
# --- name is actually removed, not just left alongside new ones ---
actual="$(run_case '
ActiveTagging::Tagging.set_tags(db, 200, ["dark-mode", "minimal"])
ActiveTagging::Tagging.set_tags(db, 200, ["retro-wave"])
tags = ActiveTagging::Tagging.tags_for(db, 200)
names = []
tags.each() do |t| names.push(t.name()) end
"#{names.length()} #{names[0]}"
')"
assert_eq "$actual" "1 retro-wave"

# --- Tagging.set_tags with [] clears every tag ---
actual="$(run_case '
ActiveTagging::Tagging.set_tags(db, 300, ["dark-mode"])
ActiveTagging::Tagging.set_tags(db, 300, [])
ActiveTagging::Tagging.tags_for(db, 300).length()
')"
assert_eq "$actual" "0"

# --- Tagging.taggable_ids_for_tag: the reverse direction -- every
# --- taggable_id currently attached to a given tag_id ---
actual="$(run_case '
ActiveTagging::Tagging.set_tags(db, 1, ["dark-mode"])
ActiveTagging::Tagging.set_tags(db, 2, ["dark-mode", "minimal"])
ActiveTagging::Tagging.set_tags(db, 3, ["minimal"])
dark_mode = ActiveTagging::Tag.find_or_create(db, "dark-mode")
ids = ActiveTagging::Tagging.taggable_ids_for_tag(db, dark_mode.id())
ids.sort().join(",")
')"
assert_eq "$actual" "1,2"

# --- two different taggable_ids sharing the same tag name reuse the
# --- same underlying Tag row (not one per owner) ---
actual="$(run_case '
ActiveTagging::Tagging.set_tags(db, 1, ["shared-tag"])
ActiveTagging::Tagging.set_tags(db, 2, ["shared-tag"])
ActiveTagging::Tag.where({"name": "shared-tag"}).count(db)
')"
assert_eq "$actual" "1"

echo "$count active_tagging tests passed"
