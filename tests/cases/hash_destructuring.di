source = {"name": "Ada", "scores": [4, 5, 6, 7], "meta": {"active": true}, "extra": 9}
{"name": name, "scores": [first, *middle, last], "meta": {"active": active}, **remaining} = source
puts(name)
puts(first)
puts(middle.join(","))
puts(last)
puts(active)
puts(remaining["extra"])

remaining["extra"] = 10
puts(source["extra"])

key = "dynamic"
{key: dynamic, **empty} = {"dynamic": nil}
puts(dynamic == nil)
puts(empty.length())

before = "unchanged"
begin
  {"present": changed, "missing": before} = {"present": "changed"}
rescue error: IndexError
  puts(error.message())
end
puts(before)

class DestructureTargets
  def assign(value)
    {"instance": @instance_value} = value
  end

  def instance_value()
    @instance_value
  end

  def self.assign_class(value)
    {"class": @@class_value} = value
  end

  def self.class_value()
    @@class_value
  end
end

targets = DestructureTargets.new()
targets.assign({"instance": 11})
DestructureTargets.assign_class({"class": 12})
puts(targets.instance_value())
puts(DestructureTargets.class_value())
