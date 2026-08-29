#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-active-social-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/active_social\"
db = SQLite3.open(\":memory:\")
db.execute(\"CREATE TABLE follows (id INTEGER PRIMARY KEY, follower_id INTEGER NOT NULL, followed_id INTEGER NOT NULL, created_at INTEGER NOT NULL)\")
ActiveSocial::Follow.configure(ActiveRecord::Repository.new(Arel.table(\"follows\"), build_follow, \"id\"))
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

# --- follows? is false until follow! creates the relationship ---
actual="$(run_case '
"#{ActiveSocial::Follow.follows?(db, 1, 2)} #{ActiveSocial::Follow.follow!(db, 1, 2)} #{ActiveSocial::Follow.follows?(db, 1, 2)}"
')"
assert_eq "$actual" "false true true"

# --- follow! is idempotent: following twice is a no-op the second time ---
actual="$(run_case '
ActiveSocial::Follow.follow!(db, 1, 2)
"#{ActiveSocial::Follow.follow!(db, 1, 2)} #{ActiveSocial::Follow.following_count(db, 1)}"
')"
assert_eq "$actual" "false 1"

# --- unfollow! is idempotent: unfollowing someone you don't follow
# is a no-op, not an error ---
actual="$(run_case '
"#{ActiveSocial::Follow.unfollow!(db, 1, 2)}"
')"
assert_eq "$actual" "false"

# --- unfollow! removes an existing relationship ---
actual="$(run_case '
ActiveSocial::Follow.follow!(db, 1, 2)
removed = ActiveSocial::Follow.unfollow!(db, 1, 2)
"#{removed} #{ActiveSocial::Follow.follows?(db, 1, 2)}"
')"
assert_eq "$actual" "true false"

# --- following/followers list the right ids, from each side
# independently -- a follow is not symmetric ---
actual="$(run_case '
ActiveSocial::Follow.follow!(db, 1, 2)
ActiveSocial::Follow.follow!(db, 1, 3)
ActiveSocial::Follow.follow!(db, 4, 2)
following = ActiveSocial::Follow.following(db, 1)
followers = ActiveSocial::Follow.followers(db, 2)
"#{following.length()} #{following.include?(2)} #{following.include?(3)} #{followers.length()} #{followers.include?(1)} #{followers.include?(4)}"
')"
assert_eq "$actual" "2 true true 2 true true"

# --- following_count/followers_count match the list lengths ---
actual="$(run_case '
ActiveSocial::Follow.follow!(db, 1, 2)
ActiveSocial::Follow.follow!(db, 1, 3)
"#{ActiveSocial::Follow.following_count(db, 1)} #{ActiveSocial::Follow.followers_count(db, 1)}"
')"
assert_eq "$actual" "2 0"

# --- following/followers are empty, not an error, for a persona with
# no relationships at all ---
actual="$(run_case '
"#{ActiveSocial::Follow.following(db, 999).length()} #{ActiveSocial::Follow.followers(db, 999).length()}"
')"
assert_eq "$actual" "0 0"

echo "$count active_social tests passed"
