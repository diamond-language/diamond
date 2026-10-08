module Mjolnir

  # A table declared in code. `fields` maps field name to a type symbol
  # (:int, :float, :string, :bool, :any); `builder` turns one result row
  # (a Hash) into an entity. Entities are plain objects -- typically a
  # `struct` -- with no persistence methods; builds come back frozen.
  class Schema
    # With `timestamps: true` the fields must include `created_at` and
    # `updated_at` (:int, epoch seconds); Repo stamps them on insert and
    # update unless the changeset sets them itself.
    def initialize(table_name: String, fields: Hash, builder: Callable[1], primary_key: String = "id", timestamps: Bool = false)
      @table_name = table_name
      @fields = fields
      @builder = builder
      @primary_key = primary_key
      @timestamps = timestamps
      @associations = {}
      @table = Arel.table(table_name)
      unless fields.has_key?(primary_key)
        raise ArgumentError.new("primary key #{primary_key} is not a field of #{table_name}")
      end
      if timestamps && !(fields.has_key?("created_at") && fields.has_key?("updated_at"))
        raise ArgumentError.new("timestamps: true needs created_at and updated_at fields on #{table_name}")
      end
    end

    def timestamps?() -> Bool = @timestamps
    def associations() -> Hash = @associations

    # Declare that `target` rows point back here through `foreign_key` (a
    # field of `target`). Loaded explicitly with Repo#preload; nothing is
    # ever fetched implicitly. Returns self so declarations chain.
    def has_many(name: String, target: Schema, foreign_key: String) -> Schema
      target.column(foreign_key)
      @associations[name] = {"kind": "has_many", "target": target, "key": foreign_key}
      self
    end

    # Declare that this schema's `foreign_key` field points at a `target` row.
    def belongs_to(name: String, target: Schema, foreign_key: String) -> Schema
      self.column(foreign_key)
      @associations[name] = {"kind": "belongs_to", "target": target, "key": foreign_key}
      self
    end

    def association(name: String)
      found = @associations[name]
      if found == nil
        raise ArgumentError.new("#{@table_name} has no association #{name}")
      end
      found
    end

    def table_name() -> String = @table_name
    def table() = @table
    def fields() -> Hash = @fields
    def field_names() -> Array = @fields.keys()
    def primary_key() -> String = @primary_key
    def field?(name) -> Bool = @fields.has_key?("#{name}")
    def type_of(name) = @fields[name]

    # The Arel column for a declared field.
    def column(name)
      unless self.field?(name)
        raise UnknownFieldError.new(@table_name, name)
      end
      @table.column("#{name}")
    end

    def build(row)
      entity = @builder(row)
      entity.freeze()
      entity
    end

    # An entity's declared fields as a Hash.
    def dump(entity) -> Hash
      values = {}
      @fields.keys().each() do |name|
        values[name] = entity.public_send(name)
      end
      values
    end

    def query() = Query.new(self, Arel.from(@table))
  end

end
