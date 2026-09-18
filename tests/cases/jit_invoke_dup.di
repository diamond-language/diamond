class Model
  def initialize(attributes: Hash = {})
    @attributes = attributes.dup()
    @association_cache = {}
  end
  def attribute(name) = @attributes[name]
end

row = {"name": "Ada", "age": 42}
m = Model.new(row)
row["name"] = "mutated"
puts(m.attribute("name"))
puts(m.attribute("age"))
