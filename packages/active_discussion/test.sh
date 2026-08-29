#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-active-discussion-package` from the repo root, which sets this
# up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/active_discussion\"
require \"$(pwd)/test_fixtures\"
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

# --- opening a discussion, and threaded comments with correct depth ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
c1 = d.create_comment!(db, "bob", "top level")
c2 = d.create_comment!(db, "carol", "a reply", c1.id())
c3 = d.create_comment!(db, "dave", "a deeper reply", c2.id())
"#{d.karma()} #{c1.depth()} #{c1.reply?()} #{c2.depth()} #{c2.reply?()} #{c3.depth()}"
')"
assert_eq "$actual" "0 0 false 1 true 2"

# --- top_level_comments/comments/comment_count/replies all agree ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
c1 = d.create_comment!(db, "bob", "top")
d.create_comment!(db, "carol", "reply one", c1.id())
d.create_comment!(db, "dave", "reply two", c1.id())
"#{d.top_level_comments(db).length()} #{d.comments(db).length()} #{d.comment_count(db)} #{c1.replies(db).length()}"
')"
assert_eq "$actual" "1 3 3 2"

# --- editing marks a comment edited; deleting removes the whole subtree ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
c1 = d.create_comment!(db, "bob", "top")
d.create_comment!(db, "carol", "reply", c1.id())
before_edit = c1.edited?()
c1.edit!(db, "top (edited)")
after_edit = c1.edited?()
before_delete = d.comment_count(db)
c1.delete!(db)
after_delete = d.comment_count(db)
"#{before_edit} #{after_edit} #{c1.body()} #{before_delete} #{after_delete}"
')"
assert_eq "$actual" "false true top (edited) 2 0"

# --- a locked discussion rejects new comments; unlocking restores it ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
d.lock!(db, "spam wave")
locked_before = d.locked?()
rejected = false
begin
  d.create_comment!(db, "bob", "hello")
rescue error: ActiveDiscussion::DiscussionLocked
  rejected = true
end
d.unlock!(db)
c = d.create_comment!(db, "bob", "hello again")
"#{locked_before} #{rejected} #{d.locked?()} #{c.persona_handle()}"
')"
assert_eq "$actual" "true true false bob"

# --- max_depth is enforced per discussion ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
d.configure_limits!(db, 1)
top = d.create_comment!(db, "bob", "top")
reply = d.create_comment!(db, "carol", "reply", top.id())
rejected = false
begin
  d.create_comment!(db, "dave", "too deep", reply.id())
rescue error: ActiveDiscussion::DepthExceeded
  rejected = true
end
"#{rejected}"
')"
assert_eq "$actual" "true"

# --- flame signals accumulate and auto-trigger a cooldown once the
# configured threshold is crossed within the configured window ---
actual="$(run_case '
db = setup_test_db()
ActiveDiscussion::Configuration.configure({"flame_threshold": 2, "flame_window": 300})
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
d.signal!(db, "spammer", "spam")
still_open = !d.cooling_down?()
d.signal!(db, "spammer", "spam")
now_cooling = d.cooling_down?()
"#{still_open} #{now_cooling} #{d.signals_for(db).length()}"
')"
assert_eq "$actual" "true true 2"

# --- karma voting is clamped and re-voting/clearing correctly reverses
# exactly what was actually applied (not a flat +-1 assumption) ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
d.vote_karma!(db, "voter1", 1)
after_first = d.karma()
d.vote_karma!(db, "voter2", 1)
after_second = d.karma()
d.vote_karma!(db, "voter1", -1)
after_flip = d.karma()
d.clear_karma_vote!(db, "voter1")
after_clear = d.karma()
"#{after_first} #{after_second} #{after_flip} #{after_clear}"
')"
assert_eq "$actual" "1 2 0 1"

# --- karma clamps at ItemKarma::MAX even with many up-votes ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
i = 0
while i < 10
  d.vote_karma!(db, "voter#{i}", 1)
  i += 1
end
d.karma()
')"
assert_eq "$actual" "5"

# --- ItemKarma visibility: hidden by default at the floor, visible to
# the author or a moderator regardless ---
actual="$(run_case '
"#{ActiveDiscussion::ItemKarma.visible?(0, "alice")} #{ActiveDiscussion::ItemKarma.visible?(-1, "alice")} #{ActiveDiscussion::ItemKarma.visible?(-1, "alice", "alice")} #{ActiveDiscussion::ItemKarma.visible?(-1, "alice", "bob", true)}"
')"
assert_eq "$actual" "true false true true"

# --- comment body validation rejects blank/oversized bodies ---
actual="$(run_case '
db = setup_test_db()
d = ActiveDiscussion::Discussion.open!(db, "skin-1")
rejected = false
begin
  d.create_comment!(db, "bob", "   ")
rescue error: ActiveRecord::ValidationError
  rejected = true
end
"#{rejected}"
')"
assert_eq "$actual" "true"

echo "$count active_discussion tests passed"
