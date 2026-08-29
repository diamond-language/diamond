module Arel

# A drop-in stand-in for a real `Statement` on a connection kind that
# doesn't support `#prepare` (Postgres, MySQL; or a plain Instance
# decorator around a real connection that doesn't forward `#prepare`,
# like `ActiveRecord::InstrumentedConnection` -- see PreparedStatements
# below) -- just forwards straight to the connection's own
# `#query`/`#execute` every time, same as before this whole class
# existed. Never cached (there's nothing worth caching), but its
# construction is cheap enough that this doesn't matter.
class UncachedStatement
  def initialize(db, sql: String)
    @db = db
    @sql = sql
  end

  def query(params = nil) = @db.query(@sql, params)
  def execute(params = nil) = @db.execute(@sql, params)
end

# Reuses one compiled `Statement` per (connection, SQL text) pair
# across every call, avoiding `sqlite3_prepare_v2`'s parse/plan cost on
# every single query -- the exact cost `db.prepare`/`Statement`
# (docs/io.md) was added to avoid, but that nothing in this framework
# actually used until now. `Query#to_a`/`#count` and
# `Insert`/`Update`/`Delete#execute`/`#to_a` all route through this
# instead of calling `db.query`/`db.execute` directly.
#
# Keyed by the connection object itself, not by anything threaded
# through Query/Repository's own public API -- safe because hashing/
# comparing a native object like `SQLite3` now uses identity (see the
# `values_equal`/`hash_value` fix this depended on); before that fix,
# using a connection as a Hash key segfaulted.
#
# Only `SQLite3` supports `#prepare` today (docs/io.md; Postgres/MySQL
# don't -- a deliberately deferred gap for those drivers), and
# `ActiveRecord::InstrumentedConnection` -- a plain Diamond `Instance`
# wrapping a real connection to log/time every call -- only forwards
# `#query`/`#execute`/`#last_insert_row_id`/`#close`, not `#prepare`
# (its whole point is instrumenting the per-call path `#prepare`
# exists to skip). `Query`/`Insert`/`Update`/`Delete` are agnostic to
# both the dialect and any such wrapper, so `.for` can't assume
# `#prepare` exists. The first call against a given connection tries
# it; a `#prepare`-less native connection raises the native "undefined
# method" `TypeError`, while a `#prepare`-less Instance (like
# InstrumentedConnection) raises `NoMethodError` instead -- a different
# class for the same underlying condition, since Diamond's own
# "undefined method" error depends on the receiver kind. Either one is
# caught here exactly once and remembered so every later call for that
# same connection skips straight to `UncachedStatement` instead of
# retrying and failing again. A real SQL error (bad syntax, a bind
# mismatch) raises `SQLite3Error`/`PostgreSQLError`/`MySQLError`, never
# `TypeError`/`NoMethodError`, so it always propagates normally rather
# than being mistaken for a missing `#prepare`.
#
# Deliberately unbounded and never evicted, per connection: the set of
# distinct SQL shapes one running app actually issues is small and
# fixed, not attacker- or input-controlled, matching this codebase's
# existing "keep it minimal" bias against premature LRU/eviction
# machinery elsewhere (see e.g. RateLimit's own fixed-window counters).
# The real cost of that: every cached entry, and the connection object
# itself (since it's a live Hash key), stays reachable for the
# lifetime of this class's own `@@cache`/`@@unsupported` -- i.e. for
# the lifetime of the whole process (or the current Thread/worker's
# own VM, since class variables aren't shared across `Thread.new`-
# spawned workers). Appropriate for this framework's own "one long-
# lived connection per worker" convention (`Database.get(context)`'s
# own memoization, reused by every example/application); NOT
# appropriate for code that opens and closes many short-lived
# connections in a loop, since each one -- and its own cached
# Statements -- would leak for the rest of the process's lifetime
# instead of being collected once closed.
class PreparedStatements
  def self.for(db, sql: String)
    if @@unsupported == nil
      @@unsupported = {}
    end
    if @@unsupported[db]
      return UncachedStatement.new(db, sql)
    end
    if @@cache == nil
      @@cache = {}
    end
    per_connection = @@cache[db]
    if per_connection == nil
      per_connection = {}
      @@cache[db] = per_connection
    end
    statement = per_connection[sql]
    if statement == nil
      begin
        statement = db.prepare(sql)
      rescue error: TypeError | NoMethodError
        # TypeError: a native connection with no #prepare at all
        # (Postgres, MySQL). NoMethodError: a plain Instance decorator
        # around a real connection that doesn't forward #prepare --
        # ActiveRecord::InstrumentedConnection only exposes #query/
        # #execute/#last_insert_row_id/#close, by design (it wraps
        # every call to log/time it, and #prepare's whole point is
        # skipping the per-call path it instruments).
        @@unsupported[db] = true
        return UncachedStatement.new(db, sql)
      end
      per_connection[sql] = statement
    end
    statement
  end
end

end
