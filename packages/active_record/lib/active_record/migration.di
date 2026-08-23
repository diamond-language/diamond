require "../../../arel/lib/arel"

module ActiveRecord

# Versioned, ordered schema changes -- the one missing piece between
# Repository/Transaction (which assume a schema already exists) and a
# real app's own evolving database. Deliberately raw SQL, not a
# dialect-aware DDL builder: Arel itself only ever renders SELECT/INSERT/
# UPDATE/DELETE, never CREATE/ALTER/DROP (see packages/arel/README.md),
# and this package follows that same scope cut rather than inventing a
# second, DDL-flavored query builder on top.
#
# A migration is a plain Hash, not a class -- the same shape Repository's
# own mapper/validator/before_save/after_save already use (an ordinary
# function reference stored as a value, invoked later). This isn't a
# style choice: a bare class name is not a passable runtime value in
# Diamond at all. `ClassName.method(...)` only resolves against a
# *literal* class name written at that exact call site, entirely at
# compile time (confirmed directly: `Foo` alone as a bare expression is
# an "undefined local variable" compile error, and `Foo.baz()` where
# `baz` isn't declared on `Foo` is a compile-time error, not a runtime
# one -- see docs/design.md's "method_missing" section). Migrator only
# ever sees a migration as one element of a generic runtime Array, so a
# class-shaped migration could never have its methods called at all.
#
#   def create_authors_up(db)
#     db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
#   end
#   def create_authors_down(db)
#     db.execute("DROP TABLE authors")
#   end
#   def create_authors_migration() = {
#     "version": "20260101120000", "up": create_authors_up, "down": create_authors_down
#   }
#
# "down" is optional -- a migration Hash with no "down" entry reads back
# nil (an ordinary missing-key lookup, not an error), and #rollback
# raises a clear error rather than silently no-opping if it's ever
# actually needed. "version" only has to be a stable, unique String --
# nothing here sorts or compares version values (Diamond's String has no
# ordering comparison at all: `<`/`>` raise TypeError, `<=>` returns nil,
# confirmed directly), so a Migrator caller's own Array order is the only
# order that matters, timestamp-shaped or not.
#
# Every method below is `self.`-owned, never instantiated -- same as
# Transaction. #run/#rollback build on Transaction.run itself (a zero-arg
# Callable), one `closure` declared fresh per loop iteration so each
# wraps that iteration's own migration and version. Getting here took
# two separate language-level fixes, both now resolved (docs/roadmap.md's
# "Language and library directions" section): a nested `def`/`closure`
# redeclared a second time inside a loop body used to raise a runtime
# TypeError on the second iteration -- needed here, one transaction per
# migration -- and, independently, a `def`/`closure` nested directly
# inside a `def self.x` method (true of every method here) used to
# type-check as a bare `Callable` rather than narrowing to the
# `Callable[0]` `Transaction.run` declares, or (once that was fixed for
# `closure` specifically) miscompiled entirely for a `closure` that also
# declares its own parameters. Plain nested `def` still can't be passed
# to `Transaction.run` this way -- it remains the `redefine_method`
# patch-factory form, unrelated to this -- but `closure` (needed here
# only for the loop-redeclaration behavior, not for any actual self/
# ivar capture -- neither callback below reads `self`) works correctly.
class Migrator
  def self.migrations_table() = Arel.table("schema_migrations")

  def self.ensure_schema_migrations_table(db)
    db.execute("CREATE TABLE IF NOT EXISTS schema_migrations (version TEXT PRIMARY KEY)")
  end

  def self.applied_versions(db, visitor = nil)
    rows = Arel.from(Migrator.migrations_table()).to_a(db, visitor)
    versions = []
    index = 0
    while index < rows.length()
      versions.push(rows[index]["version"])
      index += 1
    end
    versions
  end

  def self.check_no_duplicate_versions(migrations: Array)
    seen = []
    index = 0
    while index < migrations.length()
      version = migrations[index]["version"]
      if seen.include?(version)
        raise ArgumentError.new("duplicate migration version: #{version}")
      end
      seen.push(version)
      index += 1
    end
  end

  def self.record_applied(db, version, visitor = nil)
    Arel.insert_into(Migrator.migrations_table()).values({"version": version}).execute(db, visitor)
  end

  def self.remove_applied(db, version, visitor = nil)
    table = Migrator.migrations_table()
    predicate = table.column("version").eq(version)
    Arel.delete_from(table).where(predicate).execute(db, visitor)
  end

  # Applies every migration in `migrations` not already recorded in
  # schema_migrations, in the Array's own given order, each inside its
  # own transaction -- commits on success, rolls back and re-raises on
  # any exception, so a failure partway through a multi-migration run
  # leaves every prior migration committed and correctly excluded from
  # the next #run. Safe to call repeatedly: already-applied migrations
  # are skipped, not re-run.
  def self.run(db, migrations: Array, visitor = nil)
    Migrator.ensure_schema_migrations_table(db)
    Migrator.check_no_duplicate_versions(migrations)
    applied = Migrator.applied_versions(db, visitor)
    index = 0
    while index < migrations.length()
      migration = migrations[index]
      version = migration["version"]
      unless applied.include?(version)
        up = migration["up"]
        closure apply_migration()
          up(db)
          Migrator.record_applied(db, version, visitor)
        end
        Transaction.run(db, apply_migration)
      end
      index += 1
    end
  end

  # Reverses the `steps` most-recently-applied migrations among
  # `migrations`, found by walking the given Array in *reverse* (the
  # caller's own order is the only ordering Migrator has -- see the
  # class comment above) and collecting the first `steps` whose version
  # is currently applied. Each reversal runs inside its own transaction.
  # A migration with no "down" entry raises rather than being silently
  # skipped -- same "fail loud, no implicit magic" stance the rest of
  # this package already takes for a missing optional hook.
  def self.rollback(db, migrations: Array, steps: Int = 1, visitor = nil)
    if steps < 1
      raise ArgumentError.new("steps must be at least 1")
    end
    Migrator.ensure_schema_migrations_table(db)
    applied = Migrator.applied_versions(db, visitor)
    to_reverse = []
    index = migrations.length() - 1
    while index >= 0 && to_reverse.length() < steps
      migration = migrations[index]
      if applied.include?(migration["version"])
        to_reverse.push(migration)
      end
      index -= 1
    end
    index = 0
    while index < to_reverse.length()
      migration = to_reverse[index]
      version = migration["version"]
      down = migration["down"]
      if down == nil
        raise RuntimeError.new("migration #{version} has no \"down\" entry (irreversible)")
      end
      closure revert_migration()
        down(db)
        Migrator.remove_applied(db, version, visitor)
      end
      Transaction.run(db, revert_migration)
      index += 1
    end
  end

  # migrations not yet recorded in schema_migrations, in the given
  # Array's own order.
  def self.pending(db, migrations: Array, visitor = nil)
    Migrator.ensure_schema_migrations_table(db)
    applied = Migrator.applied_versions(db, visitor)
    result = []
    index = 0
    while index < migrations.length()
      migration = migrations[index]
      unless applied.include?(migration["version"])
        result.push(migration)
      end
      index += 1
    end
    result
  end

  # migrations from `migrations` that are already recorded in
  # schema_migrations, in the given Array's own order.
  def self.applied(db, migrations: Array, visitor = nil)
    Migrator.ensure_schema_migrations_table(db)
    applied_versions = Migrator.applied_versions(db, visitor)
    result = []
    index = 0
    while index < migrations.length()
      migration = migrations[index]
      if applied_versions.include?(migration["version"])
        result.push(migration)
      end
      index += 1
    end
    result
  end
end

end
