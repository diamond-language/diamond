# active_tagging

Normalize tag names and manage tags on records using `active_record`.

## Installation

`active_tagging` is not published to the registry yet. Until it is, copy `packages/active_tagging` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/active_tagging/`, then load it with `require_cut "active_tagging"`. `facet update` leaves hand-copied cuts in place.

It depends on `active_record`, which is published. Add it with `facet`:

```sh
facet add active_record --registry https://cuts.dilang.tech --version "^0.19.0"
facet update
```

## Usage

```ruby
require_cut "active_tagging"

db = SQLite3.open(":memory:")
db.execute("CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)")
db.execute("CREATE TABLE taggings (
  id INTEGER PRIMARY KEY,
  tag_id INTEGER NOT NULL,
  taggable_id INTEGER NOT NULL,
  UNIQUE(taggable_id, tag_id)
)")

ActiveTagging::Tag.configure(ActiveRecord::Repository.new(
  Arel.table("tags"), build_active_tagging_tag, "id", nil,
  build_active_tagging_tag_validator()))
ActiveTagging::Tagging.configure(ActiveRecord::Repository.new(
  Arel.table("taggings"), build_active_tagging_tagging, "id"))

post_id = 42   # the id of any record you want to tag
# From a free-typed form field: "Dark Mode, Minimal" -> ["dark-mode", "minimal"]
names = ActiveTagging::Tag.parse_names("Dark Mode, Minimal")
ActiveTagging::Tagging.set_tags(db, post_id, names)

# Reading them back (Tag models):
tags = ActiveTagging::Tagging.tags_for(db, post_id)
tags.map() do |t| t.name() end        # => ["dark-mode", "minimal"]

# Find the ids of everything tagged "dark-mode":
tag = ActiveTagging::Tag.find_or_create(db, "dark-mode")
ActiveTagging::Tagging.taggable_ids_for_tag(db, tag.id())   # => [42]
```

## Notes

Configure `Tag` and `Tagging` with repositories before use. `taggable_id` is an opaque ID; map returned IDs to your own records. Add indexes on `taggings(taggable_id)` and `taggings(tag_id)` for lookups.
