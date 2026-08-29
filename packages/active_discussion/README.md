# active_discussion

Threaded comments with Slashdot-style item karma and a lightweight
flame-signal/cooldown mechanism -- ported from MaquinasStack's private
Ruby `ActiveDiscussion` gem (`active_discussion/`).

## What's ported, what isn't

**Ported**: `ActiveDiscussion::Discussion` (thread root, opened
against an opaque `recording_id`), `ActiveDiscussion::Comment`
(threaded via `parent_id`/`depth`), `ActiveDiscussion::ItemKarma`
(clamp/label/visibility rules), karma voting
(`vote_karma!`/`clear_karma_vote!`, clamp-aware delta tracking so a
re-vote or a clear reverses exactly what was actually applied, not a
flat +-1), and the one moderation primitive kept from the wider
MaquinasStack review: flame signals (`signal!`) with an auto-cooldown
once the recent-signal count crosses a configured threshold within a
configured window.

**Not ported**:
- The Ruby original's full case/vote moderation tribunal
  (`active_moderation`) and `mod_forum`'s own separate permission
  scheme. The architecture review this port is based on found three
  overlapping moderation systems in MaquinasStack; this package keeps
  only the one primitive ModArtist's own code actually uses
  (report/flame-signal), not all three.
- The canvas/pinboard fields the Ruby original's `Comment` carries on
  every row (`canvas_id`, `z_index`, `spin_deg`, `x`, `y`), inherited
  from `ActiveStenographer`'s "recording placed on a canvas" storage
  primitive. Meaningless for a plain threaded comment -- a leaky UI
  metaphor the review flagged, not a feature cut for expedience.
- The materialized-path subtree query the Ruby original relies on for
  ancestry (`ActiveStenographer` gives every recording a path column
  for free). This package has no such column, so subtree lookups walk
  `parent_id` instead -- see the compiler-bug note below for why that
  walk is written recursively rather than iteratively.

## Diamond-specific notes

- Namespaced under `module ActiveDiscussion`, not plain top-level
  classes -- see `packages/active_auth/README.md`'s own note on why
  (Diamond's flat global namespace, unlike each separate Ruby host
  app's own process).
- `ItemKarma` stays decoupled from `packages/active_karma`. A
  discussion's/comment's starting karma comes from a **configured
  callable** (`Configuration.item_karma_initial_for()`), never a
  direct call into `ActiveKarma::PersonaState`. `ItemKarma` does
  provide `self.initial_from_persona_state(state)` for a host app that
  *does* want to bridge the two packages together -- but that bridging
  happens in the host app's own configured callable, not inside this
  package. This matches the Ruby original's own loose coupling
  exactly.
- `Model.self.create(db, attributes)` bypasses a model's own
  `#initialize` entirely (it calls straight into
  `Repository#create`), so an `initialize`-computed default (e.g.
  "default `created_at` to `Time.now()` if absent") never fires for
  `.create()`-based construction -- only for `.new()` + `.save()`.
  `Discussion#signal!` passes `"created_at"` explicitly to
  `FlameSignal.create(...)` for this reason; every other `.create()`
  call site in this package was checked and already passes every
  NOT-NULL column explicitly.
- `Array#sort_by` can't compare a tuple/array sort key
  (`[comment.created_at(), comment.id()]` raises a runtime type
  error) -- every multi-key sort in this package (`Discussion#comments`,
  `#top_level_comments`, `Comment#replies`) combines the keys into one
  numeric value instead: `comment.created_at() * 1000000000 + comment.id()`.

### A real compiler bug, and why subtree collection is recursive

`Comment#subtree_ids` (every descendant id, this comment included) was
originally written the obvious iterative way: seed an `Array` with the
root id, walk it with a `while` loop, and for each id already in the
array, `.where({"parent_id": id}).to_a(db)` for its children and
`.push()` each child's id onto the same array so the loop picks them up
too (a standard BFS-via-growing-array).

That pattern triggers a real Diamond compiler bug: a value that was
**pushed onto an Array from inside a loop, derived from a `Repository`
query's own mapped results, then read back out of the array by index
and fed into a *second* query in that same loop** raises a spurious
runtime "type error" at the second query call -- even though the value
being passed is a perfectly ordinary `Int`. This was reproduced in
isolation with a minimal, unrelated `ActiveRecord::Model`, and survived
every variation tried: extracting the index-read to a local, extracting
the pushed value to a local, using a `Hash`-based visited-set instead
of an `Array`. Only switching from **iteration to recursion** --
a plain top-level function calling itself, so each call gets its own
fresh local scope instead of one shared array read back across loop
iterations -- sidesteps it. See `collect_subtree_ids` at the bottom of
`lib/active_discussion/comment.di` for the working recursive version.

This bug has not been root-caused inside the compiler itself, has not
been filed, and has no dedicated regression test yet -- it's documented
here and inline as a code comment because of how easy it would be to
reintroduce by "simplifying" the recursion back to a loop.

## Required tables

```sql
CREATE TABLE active_discussions (
  id INTEGER PRIMARY KEY,
  recording_id TEXT NOT NULL UNIQUE,
  persona_handle TEXT, title TEXT, body TEXT,
  karma INTEGER NOT NULL,
  max_depth INTEGER, karma_floor TEXT,
  locked INTEGER NOT NULL DEFAULT 0, locked_at INTEGER, locked_reason TEXT,
  cooldown_until INTEGER
)

CREATE TABLE active_discussion_comments (
  id INTEGER PRIMARY KEY,
  discussion_id INTEGER NOT NULL, parent_id INTEGER,
  persona_handle TEXT NOT NULL, body TEXT NOT NULL,
  karma INTEGER NOT NULL, depth INTEGER NOT NULL,
  edited_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER
)

CREATE TABLE active_discussion_signals (
  id INTEGER PRIMARY KEY,
  discussion_id INTEGER NOT NULL, persona_handle TEXT NOT NULL,
  signal_type TEXT NOT NULL, flagged_by TEXT, created_at INTEGER NOT NULL
)

CREATE TABLE active_discussion_karma_votes (
  id INTEGER PRIMARY KEY,
  target_type TEXT NOT NULL, target_id INTEGER NOT NULL, voter_handle TEXT NOT NULL,
  value INTEGER NOT NULL, applied_delta INTEGER NOT NULL
)
```

A consuming app should add indexes on `discussion_comments.discussion_id`,
`.parent_id`, `discussion_signals.discussion_id`, and a
`UNIQUE(target_type, target_id, voter_handle)` on `karma_votes`.

## Usage

```ruby
require "../../active_discussion/lib/active_discussion"

ActiveDiscussion::Discussion.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussions"), build_discussion, "id"))
ActiveDiscussion::Comment.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_comments"), build_comment, "id"))
ActiveDiscussion::FlameSignal.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_signals"), build_flame_signal, "id"))
ActiveDiscussion::KarmaVote.configure(ActiveRecord::Repository.new(
  Arel.table("active_discussion_karma_votes"), build_karma_vote, "id"))
ActiveDiscussion::Configuration.configure()

d = ActiveDiscussion::Discussion.open!(db, "skin-42", "alice")
c1 = d.create_comment!(db, "bob", "nice work")
c2 = d.create_comment!(db, "carol", "agreed", c1.id())   # reply, depth 1

d.vote_karma!(db, "dave", 1)          # d.karma() => 1
c1.vote_karma!(db, "eve", 1)          # c1.karma() => 1

d.signal!(db, "spammer", "spam")      # flame signal; auto cool_down! once
                                       # Configuration.flame_threshold() is hit

c1.delete!(db)                        # deletes c1 and c2 (its whole subtree)
```

## Test

```sh
make test-active-discussion-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
