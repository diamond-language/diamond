# Shared fixture for test.sh's own inline test cases.
def setup_test_db()
  db = SQLite3.open(":memory:")
  db.execute([
    "CREATE TABLE active_discussions (id INTEGER PRIMARY KEY,",
    "recording_id TEXT NOT NULL UNIQUE, persona_handle TEXT, title TEXT, body TEXT,",
    "karma INTEGER NOT NULL, max_depth INTEGER, karma_floor TEXT,",
    "locked INTEGER NOT NULL DEFAULT 0, locked_at INTEGER, locked_reason TEXT,",
    "cooldown_until INTEGER)"
  ].join(" "))
  db.execute([
    "CREATE TABLE active_discussion_comments (id INTEGER PRIMARY KEY,",
    "discussion_id INTEGER NOT NULL, parent_id INTEGER, persona_handle TEXT NOT NULL,",
    "body TEXT NOT NULL, karma INTEGER NOT NULL, depth INTEGER NOT NULL,",
    "edited_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER)"
  ].join(" "))
  db.execute([
    "CREATE TABLE active_discussion_signals (id INTEGER PRIMARY KEY,",
    "discussion_id INTEGER NOT NULL, persona_handle TEXT NOT NULL,",
    "signal_type TEXT NOT NULL, flagged_by TEXT, created_at INTEGER NOT NULL)"
  ].join(" "))
  db.execute([
    "CREATE TABLE active_discussion_karma_votes (id INTEGER PRIMARY KEY,",
    "target_type TEXT NOT NULL, target_id INTEGER NOT NULL, voter_handle TEXT NOT NULL,",
    "value INTEGER NOT NULL, applied_delta INTEGER NOT NULL)"
  ].join(" "))

  ActiveDiscussion::Discussion.configure(ActiveRecord::Repository.new(Arel.table("active_discussions"), build_discussion, "id"))
  ActiveDiscussion::Comment.configure(ActiveRecord::Repository.new(Arel.table("active_discussion_comments"), build_comment, "id"))
  ActiveDiscussion::FlameSignal.configure(ActiveRecord::Repository.new(Arel.table("active_discussion_signals"), build_flame_signal, "id"))
  ActiveDiscussion::KarmaVote.configure(ActiveRecord::Repository.new(Arel.table("active_discussion_karma_votes"), build_karma_vote, "id"))
  ActiveDiscussion::Configuration.configure()
  db
end
