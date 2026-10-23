module Mjolnir

  # A table declared in code. `fields` maps field name to a type symbol
  # (:int, :float, :string, :bool, :any); `builder` turns one result row
  # (a Hash) into an entity. Entities are plain objects -- typically a
  # `struct` -- with no persistence methods; builds come back frozen.
  class Schema
    # `primary_key` is a field name, or an Array of field names for a
    # composite key (a join table keyed by its two foreign keys). A
    # composite key has no autoincrement: the caller supplies every part.
    # With `timestamps: true` the fields must include `created_at` and
    # `updated_at` (:int, epoch seconds); Repo stamps them on insert and
    # update unless the changeset sets them itself.
    def initialize(table_name: String, fields: Hash, builder: Callable[1], primary_key: String | Array = "id", timestamps: Bool = false)
      @table_name = table_name
      @fields = fields
      @builder = builder
      @primary_keys = if primary_key.is_a?(Array) then primary_key else [primary_key] end
      @primary_key = primary_key
      @timestamps = timestamps
      @associations = {}
      @table = Arel.table(table_name)
      if @primary_keys.empty?()
        raise ArgumentError.new("#{table_name} needs a primary key")
      end
      @primary_keys.each() do |key|
        unless fields.has_key?(key)
          raise ArgumentError.new("primary key #{key} is not a field of #{table_name}")
        end
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

    # The single primary key's field name; a composite key has no one name,
    # so asking for it is an error (use primary_keys).
    def primary_key() -> String
      if @primary_keys.length() != 1
        raise ArgumentError.new("#{@table_name} has a composite primary key; use primary_keys")
      end
      @primary_keys[0]
    end

    def primary_keys() -> Array = @primary_keys
    def composite?() -> Bool = @primary_keys.length() > 1

    # The Arel conditions selecting one row by key. `id` is the key's value,
    # or for a composite key an Array of the parts in primary_keys order.
    def key_predicates(id) -> Array
      values = if self.composite?() then id else [id] end
      unless values.is_a?(Array) && values.length() == @primary_keys.length()
        raise ArgumentError.new("#{@table_name} needs #{@primary_keys.length()} key values, in the order #{@primary_keys.join(", ")}")
      end
      predicates = []
      index = 0
      while index < @primary_keys.length()
        predicates.push(self.column(@primary_keys[index]).eq(values[index]))
        index += 1
      end
      predicates
    end

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

    # An entity's declared fields as a Hash. An entity built as a plain
    # Hash row (a builder that returns the row itself) is read by key.
    def dump(entity) -> Hash
      values = {}
      @fields.keys().each() do |name|
        values[name] = if entity is Hash then entity[name] else entity.public_send(name) end
      end
      values
    end

    def query() = Query.new(self, Arel.from(@table))
  end

end
