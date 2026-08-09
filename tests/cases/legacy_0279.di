module Query
 def valid?(value) = value == 42
 module_function valid?
end
Query.valid?(42)
