module ActiveRecord

# Explicit association metadata and batch preloader. Diamond ActiveRecord
# does not infer model names or inspect class macros; a repository registers
# the target repository and join columns directly.
class AssociationReflection
  attr_reader name: String
  attr_reader macro: String
  attr_reader target_repository: Repository
  attr_reader foreign_key: String
  attr_reader owner_key: String

  def initialize(name: String, macro: String, target_repository: Repository,
                 foreign_key: String, owner_key: String = "id")
    if macro != "belongs_to" && macro != "has_one" && macro != "has_many"
      raise ArgumentError.new("association macro must be belongs_to, has_one, or has_many")
    end
    @name = name
    @macro = macro
    @target_repository = target_repository
    @foreign_key = foreign_key
    @owner_key = owner_key
  end

  def belongs_to?() -> Bool = @macro == "belongs_to"
  def has_one?() -> Bool = @macro == "has_one"
  def has_many?() -> Bool = @macro == "has_many"
  def polymorphic?() -> Bool = false
  def through_reflection() = nil

  # `scope`, when supplied, is a Relation for the target repository. This is
  # the hook higher-level query planners use to project target columns and to
  # carry nested includes while retaining one batched association query.
  def preload(db, records: Array, scope = nil)
    if records.length() == 0
      return records
    end
    keys = []
    index = 0
    while index < records.length()
      key = if self.belongs_to?()
        records[index].read_attribute(@foreign_key)
      else
        records[index].read_attribute(@owner_key)
      end
      if key != nil && !keys.include?(key)
        keys.push(key)
      end
      index += 1
    end

    targets = []
    if keys.length() > 0
      table = @target_repository.table()
      lookup_column = if self.belongs_to?() then @owner_key else @foreign_key end
      target_scope = if scope == nil then @target_repository.relation() else scope end
      targets = target_scope.where(table.column(lookup_column).in_list(keys)).to_a(db)
    end

    index = 0
    while index < records.length()
      record = records[index]
      owner_value = if self.belongs_to?()
        record.read_attribute(@foreign_key)
      else
        record.read_attribute(@owner_key)
      end
      matches = []
      target_index = 0
      while target_index < targets.length()
        target_value = if self.belongs_to?()
          targets[target_index].read_attribute(@owner_key)
        else
          targets[target_index].read_attribute(@foreign_key)
        end
        if target_value == owner_value
          matches.push(targets[target_index])
        end
        target_index += 1
      end
      value = if self.has_many?()
        matches
      elsif matches.length() == 0
        nil
      else
        matches[0]
      end
      record.set_preloaded_association(@name, value)
      index += 1
    end
    records
  end
end

end
