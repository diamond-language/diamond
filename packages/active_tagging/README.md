# active_tagging

Normalize tag names and manage tags on records using `active_record`.

## Installation

Install the cut at `cuts/active_tagging/` and load it with `require_cut "active_tagging"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `active_record`.

## SQL schema

```sql
CREATE TABLE tags (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE
);
CREATE TABLE taggings (
  id INTEGER PRIMARY KEY,
  tag_id INTEGER NOT NULL,
  taggable_id INTEGER NOT NULL,
  UNIQUE(taggable_id, tag_id)
);
CREATE INDEX taggings_taggable_idx ON taggings(taggable_id);
CREATE INDEX taggings_tag_idx ON taggings(tag_id);
```

## Usage

```ruby
require_cut "active_tagging"

ActiveTagging::Tag.configure(ActiveRecord::Repository.new(
  Arel.table("tags"), build_active_tagging_tag, "id", nil,
  build_active_tagging_tag_validator()))
ActiveTagging::Tagging.configure(ActiveRecord::Repository.new(
  Arel.table("taggings"), build_active_tagging_tagging, "id"))

# On create/update, from a free-typed form field:
names = ActiveTagging::Tag.parse_names(params["tags"])   # "Dark Mode, Minimal" -> ["dark-mode", "minimal"]
ActiveTagging::Tagging.set_tags(db, record.id(), names)

# Reading them back:
tags = ActiveTagging::Tagging.tags_for(db, record.id())     # -> [Tag, Tag]

# Find record IDs tagged "dark-mode":
tag = ActiveTagging::Tag.find_or_create(db, "dark-mode")
record_ids = ActiveTagging::Tagging.taggable_ids_for_tag(db, tag.id())
```

## Notes

Configure `Tag` and `Tagging` with repositories before use. `taggable_id` is an opaque ID; map returned IDs to your own records.
