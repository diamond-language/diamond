module Mjolnir

  # Validation and change tracking as a value. `Changeset.cast` whitelists
  # and type-casts untrusted attributes; each validate_* returns a new
  # changeset with errors added. Nothing here touches the database: a Repo
  # refuses an invalid changeset, and maps a database constraint failure
  # onto a field error for each `unique_constraint` declared here.
  class Changeset
    def initialize(schema: Schema, entity, data: Hash, changes: Hash, errors: Hash, constraints: Array)
      @schema = schema
      @entity = entity
      @data = data
      @changes = changes
      @errors = errors
      @constraints = constraints
    end

    # `entity` is nil for a new row, or the loaded entity being changed.
    # Only `permitted` fields are read from `attrs`; "" casts to nil.
    def self.cast(schema: Schema, entity, attrs: Hash, permitted: Array) -> Changeset
      data = if entity == nil then {} else schema.dump(entity) end
      changes = {}
      errors = {}
      permitted.each() do |permitted_name|
        name = "#{permitted_name}"
        unless schema.field?(name)
          raise UnknownFieldError.new(schema.table_name(), name)
        end
        key = if attrs.has_key?(name) then name else nil end
        if key == nil then next end
        raw = attrs[key]
        if raw == ""
          raw = nil
        end
        cast = Changeset.cast_value(schema.type_of(name), raw)
        if cast[0]
          if cast[1] != data[name] || entity == nil
            changes[name] = cast[1]
          end
        else
          errors[name] = ["is invalid"]
        end
      end
      Changeset.new(schema, entity, data, changes, errors, [])
    end

    # [ok, value]. nil always casts (required-ness is a validation).
    def self.cast_value(type, raw) -> Array
      if raw == nil || type == :any
        return [true, raw]
      end
      if type == :string
        return [raw is String, raw]
      end
      if type == :int
        if raw is Int then return [true, raw] end
        if raw is String && Regexp.new("\\A-?[0-9]+\\z").match?(raw) then return [true, raw.to_i()] end
        return [false, nil]
      end
      if type == :float
        if raw is Float then return [true, raw] end
        if raw is Int then return [true, raw.to_f()] end
        if raw is String && Regexp.new("\\A-?[0-9]+(\\.[0-9]+)?\\z").match?(raw) then return [true, raw.to_f()] end
        return [false, nil]
      end
      if type == :bool
        if raw == true || raw == "true" || raw == "1" || raw == 1 then return [true, true] end
        if raw == false || raw == "false" || raw == "0" || raw == 0 then return [true, false] end
        return [false, nil]
      end
      raise ArgumentError.new("unknown field type #{type}")
    end

    def schema() -> Schema = @schema
    def entity() = @entity
    def changes() -> Hash = @changes
    def errors() -> Hash = @errors
    def constraints() -> Array = @constraints
    def valid?() -> Bool = @errors.empty?()
    def new_record?() -> Bool = @entity == nil

    # The pending value of a field: its change, else its current value.
    def get_field(name)
      key = "#{name}"
      if @changes.has_key?(key) then @changes[key] else @data[key] end
    end

    # Current values with every change applied.
    def apply_changes() -> Hash = @data.merge(@changes)

    def put_change(name, value) -> Changeset
      key = "#{name}"
      @schema.column(key)
      change = {}
      change[key] = value
      changes = @changes.merge(change)
      Changeset.new(@schema, @entity, @data, changes, @errors, @constraints)
    end

    def add_error(name, message: String) -> Changeset
      key = "#{name}"
      existing = if @errors.has_key?(key) then @errors[key] else [] end
      added = {}
      added[key] = existing + [message]
      errors = @errors.merge(added)
      Changeset.new(@schema, @entity, @data, @changes, errors, @constraints)
    end

    def validate_required(names: Array) -> Changeset
      result = self
      names.each() do |name|
        if result.get_field(name) == nil
          result = result.add_error(name, "can't be blank")
        end
      end
      result
    end

    def validate_length(name, min = nil, max = nil) -> Changeset
      value = self.get_field(name)
      if value == nil then return self end
      length = value.length()
      if min != nil && length < min
        return self.add_error(name, "should be at least #{min} characters")
      end
      if max != nil && length > max
        return self.add_error(name, "should be at most #{max} characters")
      end
      self
    end

    # `pattern` is a Regexp source string.
    def validate_format(name, pattern: String, message: String = "has invalid format") -> Changeset
      value = self.get_field(name)
      if value == nil || Regexp.new(pattern).match?(value) then return self end
      self.add_error(name, message)
    end

    def validate_inclusion(name, allowed: Array) -> Changeset
      value = self.get_field(name)
      if value == nil || allowed.include?(value) then return self end
      self.add_error(name, "is not included in the list")
    end

    def validate_number(name, min = nil, max = nil) -> Changeset
      value = self.get_field(name)
      if value == nil then return self end
      if min != nil && value < min
        return self.add_error(name, "must be at least #{min}")
      end
      if max != nil && value > max
        return self.add_error(name, "must be at most #{max}")
      end
      self
    end

    # Declare that a failed UNIQUE constraint on `name` is a validation
    # error, not an exception. `constraint_name` is only needed where the
    # database reports a constraint's name rather than its column (PostgreSQL).
    def unique_constraint(name, constraint_name = nil, message: String = "has already been taken") -> Changeset
      @schema.column(name)
      declared = @constraints + [{"field": "#{name}", "constraint": constraint_name, "message": message}]
      Changeset.new(@schema, @entity, @data, @changes, @errors, declared)
    end
  end

end
