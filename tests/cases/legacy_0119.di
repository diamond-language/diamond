interface Greetable
 def greet(name)
end
def accept(value: Greetable)
 value
end
def dynamic(values: Array)
 begin
  accept(values[0])
 rescue error: TypeError
  42
 end
end
dynamic([1])
