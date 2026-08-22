require "../../../arel/lib/arel"

module ActiveRecord

# Many-to-many via an explicit join table -- no naming convention
# resolves it (the join table, its two foreign-key column names, and the
# target repository are all supplied directly, the same explicit shape
# HasMany/HasOne/BelongsTo already use). Unlike those three, this can't
# just call the target repository's own #where (which only ever filters
# on the target's own table), since resolving the association means
# joining the join table to the target table and filtering on the join
# table's owner-key column -- so this builds that Arel query itself,
# using the target repository's own table/mapper/visitor (see its three
# read-only accessors above) to render and map results consistently with
# how the rest of that repository already works.
class HasManyThrough
  def initialize(target_repository: Repository, join_table: Arel::Table,
                 join_owner_key: String, join_target_key: String)
    @target_repository = target_repository
    @join_table = join_table
    @join_owner_key = join_owner_key
    @join_target_key = join_target_key
  end

  def all(db, owner_id)
    target_table = @target_repository.table()
    join_predicate = @join_table.column(@join_target_key).eq(
      target_table.column(@target_repository.id_column()))
    query = Arel.from(@join_table).join(target_table, join_predicate)
    query = query.where(@join_table.column(@join_owner_key).eq(owner_id))
    query = query.project([target_table.star()])
    rows = query.to_a(db, @target_repository.visitor())
    mapper = @target_repository.mapper()
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(mapper(rows[index]))
      index += 1
    end
    mapped
  end

  # Batch form of #all -- one join query for every owner_id instead of
  # one join query per owner. Returns a Hash of owner_id -> Array of
  # mapped target records, an empty Array for an owner_id with none.
  # Projects the join table's own owner-key column alongside the target
  # table's star purely to know which owner each returned row belongs to
  # when grouping -- a narrow, deliberate addition to the star projection
  # #all already uses, safe as long as the join table's owner-key column
  # name doesn't collide with one of the target table's own column names.
  def preload(db, owner_ids: Array)
    target_table = @target_repository.table()
    join_predicate = @join_table.column(@join_target_key).eq(
      target_table.column(@target_repository.id_column()))
    query = Arel.from(@join_table).join(target_table, join_predicate)
    query = query.where(@join_table.column(@join_owner_key).in_list(owner_ids))
    query = query.project([@join_table.column(@join_owner_key), target_table.star()])
    rows = query.to_a(db, @target_repository.visitor())
    mapper = @target_repository.mapper()
    grouped = {}
    index = 0
    while index < owner_ids.length()
      grouped[owner_ids[index]] = []
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@join_owner_key]].push(mapper(row))
      row_index += 1
    end
    grouped
  end
end

end
