b = ProgramBuilder.new()
i = 0
while i < 255
  index = b.declare_module("M#{i}")
  raise "wrong index" unless index == i
  i += 1
end
class_index = b.declare_class("Box", -1)
b.include_module(class_index, 254)
b.include_module_in_module(253, 254)
b.declare_module_field(254, "value")
begin
  b.declare_module("Overflow")
  false
rescue error: TypeError
  error.message().include?("too many modules: a program holds at most 255")
end
