b = ProgramBuilder.new()
module_index = b.declare_module("Values")
function_index = b.declare_function("value", 1, 1)
b.declare_module_method(module_index, "value", function_index, 0, 0, false)
