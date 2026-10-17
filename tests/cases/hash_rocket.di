# {"key" => value} is the same hash literal as {"key": value}; both styles can be mixed.
h = {"a" => 1, "b" => 2}
puts(h["a"] + h["b"])
puts(h.to_s())
m = {"x": 1, "y" => 2, 3 => "three", :s => [1]}
puts(m.to_s())
puts(m[3])
n = {
  "k" => {"inner" => true},
  "z" => nil,
}
puts(n.to_s())
def f(x: Hash[String, Int]) = x["q"]
puts(f({"q" => 7}))
puts({}.length())
