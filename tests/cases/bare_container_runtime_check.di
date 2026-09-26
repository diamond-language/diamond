# A bare Array or Hash says nothing about its elements, so passing one
# where Array[String] is expected is checked when the call runs rather
# than rejected at compile time -- the same as an untyped value.
def names(values: Array) -> Array[String] = values
def counts(table: Hash) -> Hash[String, Int] = table
ok = [names(["a", "b"]), counts({"x": 1})]
message = ""
begin
  names([1])
rescue e: TypeError
  message = e.message()
end
[ok, message]
