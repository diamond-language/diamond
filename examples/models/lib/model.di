# Record classes whose fields come from data. `Book.fields(spec)` generates
# Book's accessors at run time from a schema Hash (read from JSON), using
# self.compile_method to build each method body and self.define_method to
# install it -- on Book only, not on Model or its other subclasses.
#
#   {"title": {"type": "String", "required": true, "max": 80},
#    "pages": {"type": "Int", "min": 1},
#    "in_print": {"type": "Bool"}}
#
# For each field that generates:  title()  title=(value)  and, for Bool
# fields, in_print?(). field_rules() returns the spec itself, which the
# shared validation below reads.

class Model
  def initialize()
    # @data: field name -> value. @changes: only fields that differ from how
    # they started.
    @data = {}
    @changes = {}
  end

  def data() -> Hash = @data

  # Field => [value before the first change, current value].
  def changes() -> Hash = @changes

  # The heart of the example. `compile_method(name, params, body, bindings)`
  # compiles a method from SOURCE TEXT, with the `bindings` Hash's entries made
  # available by name inside the body (e.g. `key`); `define_method(name,
  # method)` installs it on this
  # class (`self` here is the subclass, e.g. Book). So each field's accessors
  # are real compiled methods, not slow dynamic lookups.
  def self.fields(spec: Hash)
    # field_rules() returns the schema itself, for validation to read.
    self.define_method("field_rules", self.compile_method("field_rules", [], "rules", {"rules": spec}))

    spec.keys().each() do |name|
      type = spec[name].fetch("type", "String")

      # The reader, and a writer that goes through `assign` for the type
      # check and change tracking.
      self.define_method(name, self.compile_method(name, [], "self.data()[key]", {"key": name}))
      self.define_method("#{name}=",
        self.compile_method("#{name}=", ["value"], "self.assign(key, kind, value)",
          {"key": name, "kind": type}))

      # Bool fields also get a predicate, e.g. `in_print?`.
      if type == "Bool"
        self.define_method("#{name}?",
          self.compile_method("#{name}?", [], "self.data()[key] == true", {"key": name}))
      end
    end
  end

  # Replaces a field's generated writer with one that sets it once and then
  # refuses changes.
  # `redefine_method` (as opposed to define_method) is for replacing a method
  # that already exists.
  def self.readonly(name: String)
    self.redefine_method("#{name}=",
      self.compile_method("#{name}=", ["value"], "self.assign_once(key, value)", {"key": name}))
  end

  # Creates a record from a Hash by calling each field's writer (so every
  # value is type-checked), then forgets the changes: a freshly built record
  # has none.
  def self.build(attributes: Hash)
    record = self.new()
    attributes.keys().each() do |key| record.public_send("#{key}=", attributes[key]) end
    record.clear_changes()
    record
  end

  def clear_changes()
    @changes = {}
  end

  # Called by every generated writer.
  def assign(field: String, type: String, value)
    # nil (unset) is always allowed; anything else must match the type.
    unless value == nil || self.matches_type?(type, value)
      raise TypeError.new("#{self.class()}.#{field} must be #{type}, got #{value.class()}")
    end

    # Change tracking: remember the ORIGINAL value of a changed field. If it
    # is set back to that original, it is no longer a change at all (so
    # editing then undoing leaves `changes` empty).
    before = @data[field]
    unless before == value
      earliest = if @changes.include_key?(field) then @changes[field][0] else before end
      if earliest == value then @changes.delete(field) else @changes[field] = [earliest, value] end
    end
    @data[field] = value
  end

  # The writer installed by `readonly`: fine while the field is unset, an
  # error after.
  def assign_once(field: String, value)
    unless @data[field] == nil
      raise FrozenError.new("#{self.class()}.#{field} is read-only once set")
    end
    self.assign(field, self.field_rules()[field].fetch("type", "String"), value)
  end

  # Messages for every rule the current values break.
  def errors() -> Array
    problems = []
    rules = self.field_rules()

    rules.keys().each() do |field|
      rule = rules[field]
      value = @data[field]

      # A missing value only matters for `required` fields; max and min are
      # checked only on values that are present and of the right type.
      if value == nil
        problems.push("#{field} is required") if rule.fetch("required", false)
      else
        if value is String && rule.include_key?("max") && value.length() > rule["max"]
          problems.push("#{field} is longer than #{rule["max"]} characters")
        end
        if value is Int && rule.include_key?("min") && value < rule["min"]
          problems.push("#{field} must be at least #{rule["min"]}")
        end
      end
    end
    problems
  end

  def valid?() -> Bool = self.errors().empty?()

  def to_s() -> String
    shown = self.field_rules().keys().map() do |field| "#{field}: #{@data[field]}" end
    "#{self.class()}(#{shown.join(", ")})"
  end

  private

  # Does `value` fit a schema type name? A Float field also accepts an Int.
  def matches_type?(type: String, value) -> Bool
    case type
    when "String" then value is String
    when "Int" then value is Int
    when "Float" then value is Float || value is Int
    when "Bool" then value is Bool
    else raise ArgumentError.new("unknown field type #{type}")
    end
  end
end
