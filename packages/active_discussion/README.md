# active_discussion

Threaded comments, karma votes, and flame-signal cooldowns backed by `active_record`.

## Installation

Install the cut at `cuts/active_discussion/` and load it with `require_cut "active_discussion"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `active_record`.

## SQL schema

```sql
CREATE TABLE active_discussions (
  id INTEGER PRIMARY KEY,
  recording_id TEXT NOT NULL UNIQUE,
  persona_handle TEXT, title TEXT, body TEXT,
  karma INTEGER NOT NULL,
  max_depth INTEGER, karma_floor TEXT,
  locked INTEGER NOT NULL DEFAULT 0, locked_at INTEGER, locked_reason TEXT,
  cooldown_until INTEGER
);

CREATE TABLE active_discussion_comments (
  id INTEGER PRIMARY KEY,
  discussion_id INTEGER NOT NULL, parent_id INTEGER,
  persona_handle TEXT NOT NULL, body TEXT NOT NULL,
  karma INTEGER NOT NULL, depth INTEGER NOT NULL,
  edited_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER
);

CREATE TABLE active_discussion_signals (
  id INTEGER PRIMARY KEY,
  discussion_id INTEGER NOT NULL, persona_handle TEXT NOT NULL,
  signal_type TEXT NOT NULL, flagged_by TEXT, created_at INTEGER NOT NULL
);

CREATE TABLE active_discussion_karma_votes (
  id INTEGER PRIMARY KEY,
  target_type TEXT NOT NULL, target_id INTEGER NOT NULL, voter_handle TEXT NOT NULL,
  value INTEGER NOT NULL, applied_delta INTEGER NOT NULL
);
```

## Usage

```ruby
require_cut "active_discussion"

ActiveDiscussion::Discussion.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussions"), build_discussion, "id"))
ActiveDiscussion::Comment.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_comments"), build_comment, "id"))
ActiveDiscussion::FlameSignal.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_signals"), build_flame_signal, "id"))
ActiveDiscussion::KarmaVote.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_karma_votes"), build_karma_vote, "id"))
ActiveDiscussion::Configuration.configure()

d = ActiveDiscussion::Discussion.open!(db, "record-42", "alice")
c1 = d.create_comment!(db, "bob", "nice work")
c2 = d.create_comment!(db, "carol", "agreed", c1.id())   # reply, depth 1

d.vote_karma!(db, "dave", 1)          # d.karma() => 1
c1.vote_karma!(db, "eve", 1)          # c1.karma() => 1

d.signal!(db, "spammer", "spam")

c1.delete!(db)                        # deletes c1 and c2 (its whole subtree)
```

## Notes

Configure the four models with repositories before use. Add indexes for discussion, parent, and signal lookups.
