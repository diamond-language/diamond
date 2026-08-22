require "../../../arel/lib/arel"

module ActiveRecord

class HasMany
  def initialize(repository: Repository, foreign_key: String,
                 owner_key: String = "id")
    @repository = repository
    @foreign_key = foreign_key
    @owner_key = owner_key
  end

  # The owner value is passed explicitly. No object introspection or naming
  # convention is involved in resolving the association.
  def all(db, owner_id)
    conditions = {}
    conditions[@foreign_key] = owner_id
    @repository.where(db, conditions)
  end

  # Batch form of #all -- one query for every owner_id instead of one
  # query per owner, avoiding the N+1 pattern a naive loop over #all
  # would produce. Returns a Hash of owner_id -> Array of mapped records;
  # every owner_id passed in gets a key (an empty Array if it has none),
  # so a caller never needs its own fallback for a missing key.
  def preload(db, owner_ids: Array)
    table = @repository.table()
    rows = Arel.from(table).where(
      table.column(@foreign_key).in_list(owner_ids)).to_a(db, @repository.visitor())
    mapper = @repository.mapper()
    grouped = {}
    index = 0
    while index < owner_ids.length()
      grouped[owner_ids[index]] = []
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@foreign_key]].push(mapper(row))
      row_index += 1
    end
    grouped
  end
end

end
