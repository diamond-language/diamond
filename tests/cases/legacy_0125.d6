interface StringMaker
 def make() -> String
end
class Wrong
 def make() -> Int
  1
 end
end
def accept(value: StringMaker)
 value
end
def dynamic(values: Array)
 begin
  accept(values[0])
 rescue error: TypeError
  42
 end
end
dynamic([Wrong.new()])
