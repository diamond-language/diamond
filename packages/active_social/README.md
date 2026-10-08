# active_social

Store and query follower relationships using `active_record`.

## Installation

`active_social` is not published to the registry yet. Until it is, copy `packages/active_social` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/active_social/`, then load it with `require_cut "active_social"`. `facet update` leaves hand-copied cuts in place.

It depends on `active_record`, which is published. Add it with `facet`:

```sh
facet add active_record --registry https://cuts.dilang.tech --version "^0.19.0"
facet update
```

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

db = SQLite3.open("app.db")
db.execute("CREATE TABLE IF NOT EXISTS follows (
  id INTEGER PRIMARY KEY,
  follower_id INTEGER NOT NULL,
  followed_id INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(follower_id, followed_id)
)")

ActiveSocial::Follow.configure(ActiveRecord::Repository.new(
  Arel.table("follows"), build_follow, "id"))

alice = 1   # ids of your own user records
bob = 2

ActiveSocial::Follow.follow!(db, alice, bob)
ActiveSocial::Follow.follows?(db, alice, bob)      # => true
ActiveSocial::Follow.following(db, alice)          # => [2]
ActiveSocial::Follow.followers(db, bob)            # => [1]
ActiveSocial::Follow.following_count(db, alice)    # => 1
ActiveSocial::Follow.unfollow!(db, alice, bob)
```

## Notes

Configure `Follow` with a repository before use. Add indexes on both ID columns and a unique constraint on `(follower_id, followed_id)`.
