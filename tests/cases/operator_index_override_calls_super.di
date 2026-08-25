class Base
  def initialize()
    @data = {}
  end
  def [](key) = @data[key]
  def []=(key, value)
    @data[key] = value
  end
end
class Child < Base
  def [](key) = super(key) * 10
end
child = Child.new()
child["a"] = 3
child["a"]
