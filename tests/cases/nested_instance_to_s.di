# Instances inside an Array or Hash print with their own to_s, as they do
# on their own; classes without one still print as #<Name>.
struct Point(x: Int, y: Int)
end
class Plain
end
p = Point.new(1, 2)
["#{[p, {"at": p}]}", "#{[Plain.new()]}"]
