# A small, chainable, adapter-agnostic SQL query builder for Diamond --
# the first slice toward a DataMapper-style persistence layer (see
# packages/arel/README.md for the full design rationale). Builds and
# renders SELECT statements only; INSERT/UPDATE/DELETE belong to a
# mapper/repository layer built on top of this, not here -- matches
# real Arel's own historical scope in Rails.
#
# ArelQuery is immutable: every chain method returns a *new* ArelQuery
# rather than mutating the receiver, so a base query can be safely
# reused as a starting point for several different queries:
#
#   base = Arel.from("people").where({"active": true})
#   adults = base.where("age >= ?", [18])
#   minors = base.where("age < ?", [18])
#
# `adults`/`minors` each see only their own added where-clause, not
# each other's -- `base` itself is never touched by either.
#
# Nothing here mentions SQLite3 (or any other adapter) by name --
# #to_a/#count only ever call `db.query(sql, params)`, the exact method
# shape the SQLite3 driver already exposes (see docs/io.md's "SQLite3"
# section). Any future adapter exposing the same #query(sql, params)
# contract is a drop-in target, the same way packages/rack stayed
# server-agnostic by depending on a shared method convention rather
# than a concrete type.

class ArelQuery
  def initialize(table, wheres, order_columns, limit_value, offset_value, select_columns)
    @table = table
    @wheres = wheres
    @order_columns = order_columns
    @limit_value = limit_value
    @offset_value = offset_value
    @select_columns = select_columns
  end

  # condition: either a Hash (ANDed equality shorthand, e.g.
  # {"active": true, "role": "admin"}) or a raw SQL fragment String
  # (e.g. "age > ?") paired with its own `params` Array. Multiple
  # .where() calls -- and multiple keys within one Hash call -- all AND
  # together; there's no OR/NOT in this v1 (see the README).
  def where(condition, params = nil)
    new_wheres = @wheres
    def add_equality(key, value)
      new_wheres = array_concat(new_wheres, [["#{key} = ?", [value]]])
    end
    if condition is Hash
      condition.each(add_equality)
    else
      bound = params
      if bound == nil
        bound = []
      end
      new_wheres = array_concat(new_wheres, [[condition, bound]])
    end
    ArelQuery.new(@table, new_wheres, @order_columns, @limit_value, @offset_value, @select_columns)
  end

  def select(columns)
    ArelQuery.new(@table, @wheres, @order_columns, @limit_value, @offset_value, columns)
  end

  # column_or_columns: a single column String ("name", or "age DESC")
  # or an Array of them -- either way, appended after whatever ordering
  # earlier .order() calls already contributed.
  def order(column_or_columns)
    columns = column_or_columns
    if !(columns is Array)
      columns = [columns]
    end
    new_order_columns = array_concat(@order_columns, columns)
    ArelQuery.new(@table, @wheres, new_order_columns, @limit_value, @offset_value, @select_columns)
  end

  def limit(n)
    ArelQuery.new(@table, @wheres, @order_columns, n, @offset_value, @select_columns)
  end

  def offset(n)
    ArelQuery.new(@table, @wheres, @order_columns, @limit_value, n, @select_columns)
  end

  # [sql_string, params_array] -- the same positional-Array-return shape
  # used elsewhere in this codebase (parsed URLs, HTTP responses, ...).
  def to_sql()
    sql = "SELECT #{@select_columns.join(", ")} FROM #{@table}"
    all_params = []
    fragments = []
    def collect_where(entry)
      fragments.push(entry[0])
      all_params = array_concat(all_params, entry[1])
    end
    if @wheres.length() > 0
      @wheres.each(collect_where)
      sql = sql + " WHERE " + fragments.join(" AND ")
    end
    if @order_columns.length() > 0
      sql = sql + " ORDER BY " + @order_columns.join(", ")
    end
    if @limit_value != nil
      sql = sql + " LIMIT #{@limit_value}"
    end
    if @offset_value != nil
      sql = sql + " OFFSET #{@offset_value}"
    end
    [sql, all_params]
  end

  # db: an already-open connection exposing #query(sql, params) --
  # see the file comment above on why nothing here names SQLite3
  # directly.
  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end

  # Wraps the *entire* rendered query (including any LIMIT/OFFSET) as a
  # subquery, so COUNT is correct regardless of which clauses are
  # present -- slightly more overhead than special-casing the common
  # case, but never wrong.
  def count(db)
    sql, params = self.to_sql()
    count_sql = "SELECT COUNT(*) AS count FROM (#{sql})"
    rows = db.query(count_sql, params)
    rows[0]["count"]
  end
end

class Arel
  def self.from(table)
    ArelQuery.new(table, [], [], nil, nil, ["*"])
  end
end
