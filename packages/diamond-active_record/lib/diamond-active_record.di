require "../../arel/lib/arel"

# Explicit persistence primitives over Arel. This package deliberately does
# not inspect schemas, infer columns, or dispatch through missing methods.
class ActiveRecordRepository
  def initialize(table: ArelTable, mapper: Callable[1], id_column: String = "id")
    @table = table
    @mapper = mapper
    @id_column = id_column
  end

  def all(db)
    rows = Arel.from(@table).to_a(db)
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  def find(db, id)
    rows = Arel.from(@table).where(@table.column(@id_column).eq(id)).take(1).to_a(db)
    if rows.length() == 0 then nil else @mapper(rows[0]) end
  end

  def where(db, column_name: String, value)
    rows = Arel.from(@table).where(@table.column(column_name).eq(value)).to_a(db)
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  def create(db, attributes: Hash)
    Arel.insert_into(@table).values(attributes).execute(db)
  end

  def update(db, id, attributes: Hash)
    statement = Arel.update(@table).set(attributes)
    statement.where(@table.column(@id_column).eq(id)).execute(db)
  end

  def delete(db, id)
    statement = Arel.delete_from(@table)
    statement.where(@table.column(@id_column).eq(id)).execute(db)
  end
end

class ActiveRecordHasMany
  def initialize(repository: ActiveRecordRepository, foreign_key: String,
                 owner_key: String = "id")
    @repository = repository
    @foreign_key = foreign_key
    @owner_key = owner_key
  end

  # The owner value is passed explicitly. No object introspection or naming
  # convention is involved in resolving the association.
  def all(db, owner_id)
    @repository.where(db, @foreign_key, owner_id)
  end
end
