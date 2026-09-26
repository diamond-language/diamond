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
    @data = {}
    @changes = {}
  end

  def data() -> Hash = @data

  # Field => [value before the first change, current value].
  def changes() -> Hash = @changes

  def self.fields(spec: Hash)
    self.define_method("field_rules", self.compile_method("field_rules", [], "rules", {"rules": spec}))
    spec.keys().each() do |name|
      type = spec[name].fetch("type", "String")
      self.define_method(name, self.compile_method(name, [], "self.data()[key]", {"key": name}))
      self.define_method("#{name}=",
        self.compile_method("#{name}=", ["value"], "self.assign(key, kind, value)",
          {"key": name, "kind": type}))
      if type == "Bool"
        self.define_method("#{name}?",
          self.compile_method("#{name}?", [], "self.data()[key] == true", {"key": name}))
      end
    end
  end

  # Replaces a field's generated writer with one that sets it once and then
  # refuses changes.
  def self.readonly(name: String)
    self.redefine_method("#{name}=",
      self.compile_method("#{name}=", ["value"], "self.assign_once(key, value)", {"key": name}))
  end

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
    unless value == nil || self.matches_type?(type, value)
      raise TypeError.new("#{self.class()}.#{field} must be #{type}, got #{value.class()}")
    end
    before = @data[field]
    unless before == value
      earliest = if @changes.include_key?(field) then @changes[field][0] else before end
      if earliest == value then @changes.delete(field) else @changes[field] = [earliest, value] end
    end
    @data[field] = value
  end

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
