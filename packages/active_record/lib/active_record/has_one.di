require "../../../arel/lib/arel"

module ActiveRecord

  class HasOne
    def initialize(repository: Repository, foreign_key: String,
                   owner_key: String = "id")
      @repository = repository
      @foreign_key = foreign_key
      @owner_key = owner_key
    end

    # Same shape as HasMany -- the owner value is passed explicitly, no
    # object introspection or naming convention resolves it -- but returns a
    # single record or nil, the same "not found" shape BelongsTo#get uses,
    # since the relationship is one-to-one on this side rather than
    # one-to-many.
    def get(db, owner_id)
      conditions = {}
      conditions[@foreign_key] = owner_id
      results = @repository.where(db, conditions)
      if results.length() == 0 then nil else results[0] end
    end

    # Batch form of #get -- one query for every owner_id. Returns a Hash of
    # owner_id -> mapped record or nil, the same "not found" shape #get
    # uses, for every owner_id passed in. If more than one row matches a
    # given owner_id (a data-integrity assumption this association doesn't
    # enforce), the last one wins -- same as #get looking at only the
    # first result, just the opposite end of an unordered result set.
    def preload(db, owner_ids: Array)
      table = @repository.table()
      rows = Arel.from(table).where(
        table.column(@foreign_key).in_list(owner_ids)).to_a(db, @repository.visitor())
      mapper = @repository.mapper()
      grouped = {}
      index = 0
      while index < owner_ids.length()
        grouped[owner_ids[index]] = nil
        index += 1
      end
      row_index = 0
      while row_index < rows.length()
        row = rows[row_index]
        grouped[row[@foreign_key]] = mapper(row)
        row_index += 1
      end
      grouped
    end
  end

end
