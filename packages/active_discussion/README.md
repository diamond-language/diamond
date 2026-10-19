# active_discussion

Threaded comments, karma votes, and flame-signal cooldowns backed by `active_record`.

## Installation

`active_discussion` is not published to the registry yet. Until it is, copy `packages/active_discussion` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/active_discussion/`, then load it with `require_cut "active_discussion"`. `facet update` leaves hand-copied cuts in place.

It depends on `active_record`, which is published. Add it with `facet`:

```sh
facet add active_record --registry https://cuts.dilang.tech --version "^0.19.0"
facet update
```

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
  value INTEGER NOT NULL, applied_delta INTEGER NOT NULL,
  UNIQUE(target_type, target_id, voter_handle)
);
```

## Usage

```ruby
require_cut "active_discussion"

db = SQLite3.open("app.db")   # with the four tables above created

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

A vote is applied by reading the voter's existing vote and the target's karma, then writing both. Two voters on the same target at the same moment each start from the karma they loaded, so one of the two changes can be lost, and the `UNIQUE(target_type, target_id, voter_handle)` constraint above is what turns a double vote by one voter into an error instead of a second row. The package has no row locking to prevent the first case. If votes on one target arrive concurrently, serialize them in the application (one writer per target, or `BEGIN IMMEDIATE` on SQLite) and recompute `karma` from the stored `applied_delta` values when it matters.
