# active_record

Repositories, associations, validation, migrations, and an optional model layer for SQL-backed Diamond applications.

## Installation

Install the cut at `cuts/active_record/` and load it with `require_cut "active_record"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `arel`.

## Usage

```diamond
require_cut "active_record"

repository = ActiveRecord::Repository.new(
  Arel.table("authors"),
  map_author,
  "id"
)
author = repository.find(db, 1)
repository.update(db, 1, {"country": "England"})
```

## Notes

Supply a row mapper when constructing a repository. Pass a dialect visitor for non-SQLite databases. See the `arel` visitor documentation for supported SQL dialects.
