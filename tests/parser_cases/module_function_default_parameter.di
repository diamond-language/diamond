module Math
 BASE = 40
 def add(value: Int = 2) -> Int = BASE + value
 module_function add
end
puts(Math.add())
puts(Math.add(1))
nil
