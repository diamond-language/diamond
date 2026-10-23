# A keyword argument may start a continuation line of a `.new` call.
class Schema
  def initialize(name, fields, builder, keys: Array = [])
    @count = keys.length()
  end
  def count() = @count
end

def build_row(row) = row

schema = Schema.new("t",
  {"a": 1}, build_row,
  keys: ["a", "b"])
schema.count()
