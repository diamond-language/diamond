def size(value: Sized) -> Int
 value.length()
end
def dynamic(values: Array)
 begin
  size(values[0])
 rescue error: TypeError
  42
 end
end
dynamic([1])
