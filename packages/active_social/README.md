# active_social

Store and query follower relationships using `active_record`.

## Installation

Install the cut at `cuts/active_social/` and load it with `require_cut "active_social"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `active_record`.

## SQL schema

```sql
CREATE TABLE follows (
  id INTEGER PRIMARY KEY,
  follower_id INTEGER NOT NULL,
  followed_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(follower_id, followed_id)
);
CREATE INDEX follows_follower_idx ON follows(follower_id);
CREATE INDEX follows_followed_idx ON follows(followed_id);
```

## Usage

```ruby
require_cut "active_social"

ActiveSocial::Follow.configure(ActiveRecord::Repository.new(
  Arel.table("follows"), build_follow, "id"))

ActiveSocial::Follow.follow!(db, alice.id(), bob.id())
ActiveSocial::Follow.follows?(db, alice.id(), bob.id())      # => true
ActiveSocial::Follow.following(db, alice.id())               # => [bob.id()]
ActiveSocial::Follow.followers(db, bob.id())                 # => [alice.id()]
ActiveSocial::Follow.following_count(db, alice.id())         # => 1
ActiveSocial::Follow.unfollow!(db, alice.id(), bob.id())
```

## Notes

Configure `Follow` with a repository before use. Add indexes on both ID columns and a unique constraint on `(follower_id, followed_id)`.
