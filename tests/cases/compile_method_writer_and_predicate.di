# compile_method accepts any name def does, including writers (name=) and
# predicates (name?), so accessors can be generated at run time.
class Record
  def initialize()
    @data = {}
  end
  def data() -> Hash = @data
end
["name", "age"].each() do |field|
  Record.define_method(field, Record.compile_method(field, [], "self.data()[key]", {"key": field}))
  Record.define_method("#{field}=",
    Record.compile_method("#{field}=", ["value"], "self.data()[key] = value", {"key": field}))
end
Record.define_method("adult?", Record.compile_method("adult?", [], "self.age() >= 18", {}))
record = Record.new()
record.name = "Ada"
record.age = 36
[record.name(), record.age(), record.adult?(), record.respond_to?(to_sym("name="))]
