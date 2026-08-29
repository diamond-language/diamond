# active_social

The persona follow graph -- ported from MaquinasStack's private Ruby
`ActiveSocial` gem (`active_social/`). Follows Alice, Bob appears in
Alice's `following` list and Alice appears in Bob's `followers` list.

## What's ported, what isn't

**Ported**: `ActiveSocial::Follow` -- `follow!`/`unfollow!` (both
idempotent, matching the Ruby original: following someone you already
follow, or unfollowing someone you don't, is a no-op returning `false`,
not an error), `follows?`, `following`/`followers` (id lists), and
`following_count`/`followers_count`.

**Not ported**: the Ruby original's reactions API (`react!`/
`unreact!`/`reacted?`/`reactors`/`reaction_count` -- arbitrary persona
-> target reactions for likes, reposts, etc.). Per the architecture
review this port is based on, this API was built but ModArtist's own
real favoriting feature bypasses it entirely with a bespoke model --
porting an unused generic layer isn't worth it. A consuming app
wanting favorites/likes should build that as its own direct feature
(the same way `applications/skindicate.dia`'s own `Comment` model is a
real, specific feature rather than a generic "reaction" abstraction),
not by resurrecting this API.

## Diamond-specific notes

- **Storage swap, not a missing feature.** The Ruby original stores
  follows as two event-sourced recordings per relationship (outbound
  under the follower, inbound under the followed, "last event wins"
  for active/inactive state) via `ActiveStenographer`, which isn't
  ported yet (see `packages/active_karma/README.md` for why porting
  just enough of it for one consumer would reproduce the foundation-
  depends-on-consumers problem the architecture review flagged). A
  plain `follows` table -- one row per active relationship, deleted on
  unfollow -- answers the exact same two query directions
  (`WHERE follower_id = x` / `WHERE followed_id = x`) without the
  double-write/event-log machinery at all. This is a simplification
  the storage swap makes possible, not a cut corner.
- `follower_id`/`followed_id` are opaque ids, exactly like the Ruby
  original's own string `persona_id`s -- this package doesn't assume
  or enforce which table they reference; a consuming app's own schema
  (typically an accounts/personas table -- see `packages/active_auth`)
  owns that foreign key relationship.
- Namespaced under `module ActiveSocial`, not a plain top-level
  `Follow` class -- see `packages/active_auth/README.md`'s own note on
  why (Diamond's flat global namespace, unlike each separate Ruby host
  app's own process).

## Required table

```sql
CREATE TABLE follows (
  id INTEGER PRIMARY KEY,
  follower_id INTEGER NOT NULL,
  followed_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL
)
-- a consuming app should also add indexes on follower_id and
-- followed_id, and typically a UNIQUE(follower_id, followed_id)
```

## Usage

```ruby
require "../../active_social/lib/active_social"

ActiveSocial::Follow.configure(ActiveRecord::Repository.new(
  Arel.table("follows"), build_follow, "id"))

ActiveSocial::Follow.follow!(db, alice.id(), bob.id())
ActiveSocial::Follow.follows?(db, alice.id(), bob.id())      # => true
ActiveSocial::Follow.following(db, alice.id())               # => [bob.id()]
ActiveSocial::Follow.followers(db, bob.id())                 # => [alice.id()]
ActiveSocial::Follow.following_count(db, alice.id())         # => 1
ActiveSocial::Follow.unfollow!(db, alice.id(), bob.id())
```

## Test

```sh
make test-active-social-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
