# Prepared-statement caching: direct evidence

`Arel::PreparedStatements` (`packages/arel/lib/arel/prepared_statements.di`)
reuses one compiled `Statement` per (connection, SQL text) pair across
every `Query#to_a`/`#count` and `Insert`/`Update`/`Delete#execute`/`#to_a`
call, instead of re-preparing the same statement shape from scratch on
every call the way plain `SQLite3#query`/`#execute` always have. This
directory measures that win directly, the same "short, single-threaded,
non-networked script that runs to completion and prints its own timing"
shape `bench/gc_churn` already established, for the same reason: no
networking, threading, or OS-scheduling noise mixed into the number.

- **`uncached_query.di`**: the pre-existing behavior, still fully
  available -- `db.query(sql, params)` directly, paying
  `sqlite3_prepare_v2`'s full parse/plan cost on every single call even
  though it's the same statement shape every time.
  ```
  ./build/diamond bench/prepared_statements/uncached_query.di ITERATIONS
  ```
- **`cached_query.di`**: the same workload through
  `Arel::PreparedStatements.for(db, sql).query(params)` instead -- the
  statement is compiled once, on the first call, and every later call
  reuses it.
  ```
  ./build/diamond bench/prepared_statements/cached_query.di ITERATIONS
  ```
- **`cached_query_many_shapes.di`**: the cached path again, but rotating
  through `SHAPE_COUNT` distinct SQL strings instead of one -- checks the
  cache still wins once a real app's own handful of distinct query
  shapes is in play, not just the single-statement best case.
  ```
  ./build/diamond bench/prepared_statements/cached_query_many_shapes.di ITERATIONS SHAPE_COUNT
  ```
- **`repository_level.di`**: context, not a before/after -- the full
  `ActiveRecord`/`Arel` stack's own per-call cost (`Query` construction,
  `SQLiteVisitor` rendering, `Hash`-per-row mapping) around a repeated
  `Repository#find`, which already goes through the cache automatically
  post-fix. There's no "uncached `Repository`" left to compare against,
  so this exists to show how much of a real call's total cost is ORM
  overhead sitting on top of the raw SQL-layer numbers above -- reading
  the caching win off *this* number instead would understate it by an
  order of magnitude (see below).
  ```
  ./build/diamond bench/prepared_statements/repository_level.di ITERATIONS
  ```

All four share `setup.di`'s fixture: a single in-memory `items` table,
200 seeded rows, queried by primary key (`WHERE id = ?`, cycling through
the 200 ids) -- the same "point lookup repeated many times" shape a real
worker's request loop actually produces.

## Results

Same machine as `bench/gc_churn` (12 logical / 6 physical cores), release
build (`make release`), single-threaded, wall time both self-reported
(`Time.monotonic()`, printed by each script) and cross-checked externally
(`/usr/bin/time -f %es`) -- the two agreed to within process-startup noise
at every data point below, so only the self-reported number is tabled.

**Iteration sweep** (`uncached_query.di` vs `cached_query.di`, one SQL
shape):

| iterations | uncached | cached | reduction |
|-----------:|---------:|-------:|----------:|
|      1,000 |  0.0035s | 0.0021s |    40.9% |
|      5,000 |  0.0179s | 0.0110s |    38.5% |
|     20,000 |  0.0680s | 0.0407s |    40.2% |
|     50,000 |  0.1646s | 0.0978s |    40.6% |
|    100,000 |  0.3327s | 0.1876s |    43.6% |

A consistent ~40% reduction in pure SQL-execution-layer time across two
orders of magnitude of iteration count, for the single most common shape
(one query, run repeatedly) this change targets.

**Distinct-shape sweep** (`cached_query_many_shapes.di`, 50,000 iterations
held fixed, only `SHAPE_COUNT` varies):

| shape count | elapsed |
|------------:|--------:|
|           1 | 0.1055s |
|          10 | 0.1172s |
|          50 | 0.1164s |
|         200 | 0.1216s |

Near-flat: going from one cached statement to 200 (each looked up by its
own SQL-text `Hash` key, per `PreparedStatements.for`) adds only ~15% on
top of the single-shape number at the same iteration count -- confirming
the design goal from the class's own doc comment ("the set of distinct
SQL shapes one running app actually issues is small and fixed") holds up
even well past what a real app's own query variety looks like.

**ORM-overhead context** (`repository_level.di`, already using the cache
internally):

| iterations | elapsed | vs. `cached_query.di` at the same iteration count |
|-----------:|--------:|---------------------------------------------------:|
|      1,000 |  0.0834s |                                     ~40x higher |
|      5,000 |  0.4066s |                                     ~37x higher |
|     20,000 |  1.6727s |                                     ~41x higher |
|     50,000 |  4.1829s |                                     ~43x higher |

The caching win is real and consistent at the SQL layer, but it's a
small slice of a real `Repository#find` call's total cost -- `Query`
object construction/copying, `SQLiteVisitor` rendering to a fresh SQL
string every call (itself not cached; only the *prepared statement* for
a given rendered string is), and mapping each result row through a
`Hash` all dominate over the now-cheaper SQL execution step itself. This
isn't a regression or a gap in the fix -- it's the honest reason this
directory reports the SQL-layer number as the headline result rather
than the `Repository`-level one, matching `bench/gc_churn`'s own
discipline of stating exactly what a number does and doesn't show
rather than the most flattering one available.

This prediction was checked against real traffic, not just guessed:
`bench/project_board_http` re-ran its own existing full-stack HTTP
benchmark after this caching work landed and saw **no measurable
end-to-end throughput change** (within ~2.4% of its pre-caching
baseline across four runs -- see that directory's own `RESULTS.md`).
HTTP parsing, routing/middleware, template rendering, and per-query
instrumentation overhead swamp the microseconds this cache saves at
real request volumes -- consistent with, not contradicted by, the
~40% SQL-layer win measured here.

## Reproducing

```
make release
for iters in 1000 5000 20000 50000 100000; do
  ./build/diamond bench/prepared_statements/uncached_query.di "$iters"
  ./build/diamond bench/prepared_statements/cached_query.di "$iters"
done
for shapes in 1 10 50 200; do
  ./build/diamond bench/prepared_statements/cached_query_many_shapes.di 50000 "$shapes"
done
for iters in 1000 5000 20000 50000; do
  ./build/diamond bench/prepared_statements/repository_level.di "$iters"
done
```

Every run here finishes in well under a second and holds nothing but a
200-row in-memory table -- no watchdog or resource limit needed, unlike
`bench/burn_in`'s own live-server pushes.
