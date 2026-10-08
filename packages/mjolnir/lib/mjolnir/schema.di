module Mjolnir

  # A table declared in code. `fields` maps field name to a type symbol
  # (:int, :float, :string, :bool, :any); `builder` turns one result row
  # (a Hash) into an entity. Entities are plain objects -- typically a
  # `struct` -- with no persistence methods; builds come back frozen.
  class Schema
    def initialize(table_name: String, fields: Hash, builder: Callable[1], primary_key: String = "id")
      @table_name = table_name
      @fields = fields
      @builder = builder
      @primary_key = primary_key
      @table = Arel.table(table_name)
      unless fields.has_key?(primary_key)
        raise ArgumentError.new("primary key #{primary_key} is not a field of #{table_name}")
      end
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
