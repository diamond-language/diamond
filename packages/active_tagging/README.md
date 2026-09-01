# active_tagging

A generic tagging system for [Diamond](https://gitlab.com/dmn9180/diamond)
— generalized from `applications/skindicate.dia`'s own hand-rolled
`Tag`/`Tagging` models and `SkinsController.parse_tags`: free-typed,
comma-separated text -> normalized tag names -> find-or-create `Tag`
rows -> diff-and-replace join rows on save.

## What's here

- `ActiveTagging::Tag` — `name` plus `.normalize_name`/`.parse_names`
  (free text -> normalized, deduplicated names) and `.find_or_create`.
- `ActiveTagging::Tagging` — the join row (`tag_id`, `taggable_id`),
  plus `.set_tags`/`.tags_for`/`.taggable_ids_for_tag`.
- `build_active_tagging_tag_validator()` — presence, 1-40 characters,
  `^[a-z0-9-]+$` (the charset `Tag.normalize_name` itself produces).

## Diamond-specific notes

- **`taggable_id` is a single opaque id**, exactly like
  `packages/active_social`'s `Follow#follower_id`/`#followed_id` — this
  package doesn't assume or enforce which table it references.
- **No `taggable_type` discriminator, unlike a Rails
  `ActsAsTaggableOn`.** One `ActiveTagging::Tagging` class means one
  shared `@@repository` class variable — two *different* taggable
  types both calling `Tagging.configure` would collide (last call
  wins, per `packages/active_record`'s own `Model` class comment on
  why). Skindicate only ever tags one thing (`Skin`), so this isn't a
  real limitation there. If a second taggable type shows up later,
  give it its own join table and its own subclass of
  `ActiveRecord::Model` mirroring `Tagging`'s own shape (four lines —
  `attr_accessor`/`initialize`/`to_attributes`/`.configure`) rather
  than trying to share this one; adding a real `taggable_type`
  discriminator now, before a second consumer exists to prove out what
  it'd actually need, would be solving a problem this package doesn't
  have yet.
- Namespaced under `module ActiveTagging`, matching every other
  `active_*` package here (`active_social`, `active_auth`, ...) — see
  `packages/active_auth/README.md`'s own note on why (Diamond's flat
  global namespace).

## Required tables

```sql
CREATE TABLE tags (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE
);
CREATE TABLE taggings (
  id INTEGER PRIMARY KEY,
  tag_id INTEGER NOT NULL,
  taggable_id INTEGER NOT NULL
);
-- a consuming app should also add an index on taggings(taggable_id)
-- and taggings(tag_id), and typically a
-- UNIQUE(taggable_id, tag_id)
```

## Usage

```ruby
require "../../active_tagging/lib/active_tagging"

ActiveTagging::Tag.configure(ActiveRecord::Repository.new(
  Arel.table("tags"), build_active_tagging_tag, "id", nil,
  build_active_tagging_tag_validator()))
ActiveTagging::Tagging.configure(ActiveRecord::Repository.new(
  Arel.table("taggings"), build_active_tagging_tagging, "id"))

# On create/update, from a free-typed form field:
names = ActiveTagging::Tag.parse_names(params["tags"])   # "Dark Mode, Minimal" -> ["dark-mode", "minimal"]
ActiveTagging::Tagging.set_tags(db, skin.id(), names)

# Reading them back:
tags = ActiveTagging::Tagging.tags_for(db, skin.id())     # -> [Tag, Tag]

# The reverse direction -- every Skin tagged "dark-mode":
tag = ActiveTagging::Tag.find_or_create(db, "dark-mode")
skin_ids = ActiveTagging::Tagging.taggable_ids_for_tag(db, tag.id())
skins = []
skin_ids.each() do |id| skins.push(Skin.find(db, id)) end
```

`taggable_ids_for_tag` returns raw ids, not mapped rows — this package
has no way to know which class `taggable_id` refers to (there's no
`taggable_type` column, see above), so mapping them through your own
owner model's `.find` is the caller's job, the same way
`applications/skindicate.dia`'s own `Entry#entryable` dispatches on its
`entryable_type` column explicitly rather than through any generic
lookup.

## What's deliberately out of scope

- **A `taggable_type` discriminator / multiple taggable types sharing
  one `Tagging` class.** See "Diamond-specific notes" above.
- **Tag clouds, popularity counts, or any other read-side aggregation
  beyond `.tags_for`/`.taggable_ids_for_tag`.** Build those as your own
  query against the `taggings` table if you need them.
- **Case-sensitive or non-`[a-z0-9-]` tag names.** `Tag.normalize_name`
  always lowercases and hyphenates; there's no opt-out.

## Test

```sh
make test-active-tagging-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
