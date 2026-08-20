# diamond-active_record

An explicit, low-magic persistence layer built on Arel.

The first slice is `ActiveRecordRepository`, which receives all metadata it
needs instead of inspecting a schema or using dynamic dispatch:

```diamond
require "../../packages/diamond-active_record/diamond-active_record"

repository = ActiveRecordRepository.new(
  Arel.table("authors"),
  map_author,
  "id"
)
author = repository.find(db, 1)
repository.update(db, 1, {"country": "England"})
```

The repository supports `all`, `find`, `create`, `update`, and `delete`.
Row-to-object mapping is supplied by the application. Associations,
validation, dirty tracking, transactions, and query scopes remain explicit
application code for now.
