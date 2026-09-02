require "../../../arel/lib/arel"

module ActiveRecord

  class BelongsTo
    def initialize(repository: Repository, owner_key: String = "id")
      @repository = repository
      @owner_key = owner_key
    end

    # The child's own foreign-key value is passed explicitly -- no object
    # introspection reads it off a child instance. Unlike
    # Repository#find (which always looks up @id_column), this
    # goes through #where so an @owner_key other than the owner repository's
    # primary key still works; nil (not an empty Array) when nothing
    # matches, same "not found" shape #find already uses.
    def get(db, foreign_key_value)
      conditions = {}
      conditions[@owner_key] = foreign_key_value
      results = @repository.where(db, conditions)
      if results.length() == 0 then nil else results[0] end
    end

    # Batch form of #get -- one query for every foreign_key_value instead
    # of one per child. Returns a Hash of foreign_key_value -> mapped owner
    # record or nil, the same "not found" shape #get uses, for every value
    # passed in.
    def preload(db, foreign_key_values: Array)
      table = @repository.table()
      rows = Arel.from(table).where(
        table.column(@owner_key).in_list(foreign_key_values)).to_a(db, @repository.visitor())
      mapper = @repository.mapper()
      grouped = {}
      index = 0
      while index < foreign_key_values.length()
        grouped[foreign_key_values[index]] = nil
        index += 1
      end
      row_index = 0
      while row_index < rows.length()
        row = rows[row_index]
        grouped[row[@owner_key]] = mapper(row)
        row_index += 1
      end
      grouped
    end
  end

end
